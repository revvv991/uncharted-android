// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "videodec2_impl.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_utils.h"
#include "videodec_error.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#include "common/support/avdec.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace Libraries::Videodec2 {

namespace {
std::atomic<u64> g_decode_ns{0}, g_slowest_decode_ns{0}, g_convert_ns{0}, g_copy_ns{0};

u64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void AddDecodeNs(u64 ns) {
    g_decode_ns += ns;
    if (ns > g_slowest_decode_ns.load(std::memory_order_relaxed)) {
        g_slowest_decode_ns = ns; // one decode thread: no lost maximum worth a CAS loop
    }
}
} // namespace

VdecStageTimes TakeVdecStageTimes() {
    return {g_decode_ns.exchange(0) / 1e9, g_slowest_decode_ns.exchange(0) / 1e6,
            g_convert_ns.exchange(0) / 1e9, g_copy_ns.exchange(0) / 1e9};
}

// The games read the chroma plane after luma rows padded to 64 (1088 for 1080p; 1472 for 1440p).
static constexpr u32 kHeightAlignment = 64;

// Frames fitted into a game display size (BB_GAME_RES) use the display's own row length (3440 for
// 3440x1440), which the games take as the pitch of the movie planes; others the usual 64.
static u32 PitchAlignment() {
    s32 w = 0, h = 0;
    GameResolution(w, h);
    return w ? 16 : 64;
}

VdecDecoder::VdecDecoder(const OrbisVideodec2DecoderConfigInfo& config_info,
                         const OrbisVideodec2DecoderMemoryInfo& memory_info) {
    const AVCodec* codec = avcodec_find_decoder(
        config_info.codec_type == OrbisVideodec2CodecType::Avc ? AV_CODEC_ID_H264
                                                               : AV_CODEC_ID_HEVC);
    ASSERT(codec);

    m_codec_context = avcodec_alloc_context3(codec);
    ASSERT(m_codec_context);
    m_codec_context->width = config_info.max_frame_width;
    m_codec_context->height = config_info.max_frame_height;
    m_max_width = config_info.max_frame_width;
    m_max_height = config_info.max_frame_height;
    m_codec_context->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
    // g2-arm64: four decoding threads (BB_VDEC_THREADS=<n>; 1: on the game's own thread, as
    // before). One thread took 9 ms of the 16.7 a 60 FPS movie frame has on the G2: a phone that
    // throttles fell behind the movie's audio (issue #6). The first picture comes n-1 decodes
    // later; the game asks for it, and flushes the rest at the end.
    const char* threads_env = std::getenv("BB_VDEC_THREADS");
    const int threads = threads_env && std::atoi(threads_env) > 0 ? std::atoi(threads_env) : 4;
    if (threads > 1) {
        m_codec_context->thread_count = threads;
        m_codec_context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }

    avcodec_open2(m_codec_context, codec, nullptr);
}

VdecDecoder::~VdecDecoder() {
    avcodec_free_context(&m_codec_context);
    sws_freeContext(m_sws_context);
    sws_freeContext(m_fit_context);
    av_frame_free(&m_nv12_frame);
}


namespace {
/// BB_VDEC_SIZE=<w>x<h>: hand every frame to the game scaled to this size. Uncharted's movie
/// player sizes its frames from the display configuration (the output size), which a resolution
/// patch lowers, while its movies are 1080p.
bool EnvOutputSize(s32& width, s32& height) {
    static const std::pair<s32, s32> size = [] {
        const char* env = std::getenv("BB_VDEC_SIZE");
        int w = 0, h = 0;
        if (!env || std::sscanf(env, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
            return std::pair<s32, s32>{0, 0};
        }
        return std::pair<s32, s32>{w & ~1, h & ~1};
    }();
    width = size.first;
    height = size.second;
    return width > 0;
}

/// BB_VDEC_CONTENT=<w>x<h>: frames keep the stream's size, the picture is scaled to this size in
/// their top-left corner (the rest black). Uncharted's movie player maps the movie's own size
/// (1920x1080, from the stream) 1:1 onto the display buffer: with a resolution patch's 1280x720
/// output only the top-left 1280x720 of the movie showed, magnified 1.5x. With
/// BB_VDEC_CONTENT=1280x720 that corner holds the whole movie.
bool EnvContentSize(s32& width, s32& height) {
    static const std::pair<s32, s32> size = [] {
        const char* env = std::getenv("BB_VDEC_CONTENT");
        int w = 0, h = 0;
        if (!env || std::sscanf(env, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
            return std::pair<s32, s32>{0, 0};
        }
        return std::pair<s32, s32>{w & ~1, h & ~1};
    }();
    width = size.first;
    height = size.second;
    return width > 0;
}

#if defined(__aarch64__)
/// g2-arm64: an area filter for the exact ratios below 1080p, 16 output pixels at a time.
struct Wide {
    uint16x8_t lo, hi;
};

/// 2 * major + minor, widened.
inline Wide Weigh21(uint8x16_t major, uint8x16_t minor) {
    return {vaddw_u8(vshll_n_u8(vget_low_u8(major), 1), vget_low_u8(minor)),
            vaddw_high_u8(vshll_high_n_u8(major, 1), minor)};
}

inline Wide Weigh21(Wide major, Wide minor) {
    return {vaddq_u16(vshlq_n_u16(major.lo, 1), minor.lo),
            vaddq_u16(vshlq_n_u16(major.hi, 1), minor.hi)};
}

/// x / 9, rounded (x <= 9 * 255): (x * 7282 + 2^15) >> 16 by the doubling multiply.
inline uint8x16_t Div9(Wide x) {
    return vcombine_u8(vqmovun_s16(vqrdmulhq_n_s16(vreinterpretq_s16_u16(x.lo), 3641)),
                       vqmovun_s16(vqrdmulhq_n_s16(vreinterpretq_s16_u16(x.hi), 3641)));
}

/// 3:2 - 16 groups of 3 pixels in 3 rows: 2 rows of 2 pixels each. An output pixel covers one
/// source pixel and half of the next on each axis (weights 2 and 1, sum / 9).
inline void Area32(const u8* r0, const u8* r1, const u8* r2, uint8x16x2_t& top,
                   uint8x16x2_t& bottom) {
    const uint8x16x3_t a = vld3q_u8(r0), b = vld3q_u8(r1), c = vld3q_u8(r2);
    Wide up[3], down[3];
    for (int k = 0; k < 3; ++k) {
        up[k] = Weigh21(a.val[k], b.val[k]);
        down[k] = Weigh21(c.val[k], b.val[k]);
    }
    top.val[0] = Div9(Weigh21(up[0], up[1]));
    top.val[1] = Div9(Weigh21(up[2], up[1]));
    bottom.val[0] = Div9(Weigh21(down[0], down[1]));
    bottom.val[1] = Div9(Weigh21(down[2], down[1]));
}

/// 2:1 - 16 pairs of pixels in 2 rows: the mean of each 2x2.
inline uint8x16_t Area21(const u8* r0, const u8* r1) {
    const uint8x16x2_t a = vld2q_u8(r0), b = vld2q_u8(r1);
    const uint16x8_t lo = vaddq_u16(vaddl_u8(vget_low_u8(a.val[0]), vget_low_u8(a.val[1])),
                                    vaddl_u8(vget_low_u8(b.val[0]), vget_low_u8(b.val[1])));
    const uint16x8_t hi =
        vaddq_u16(vaddl_high_u8(a.val[0], a.val[1]), vaddl_high_u8(b.val[0], b.val[1]));
    return vcombine_u8(vrshrn_n_u16(lo, 2), vrshrn_n_u16(hi, 2));
}

/// 3:1 - 16 groups of 3 pixels in 3 rows: the mean of each 3x3.
inline uint8x16_t Area31(const u8* r0, const u8* r1, const u8* r2) {
    const uint8x16x3_t rows[3] = {vld3q_u8(r0), vld3q_u8(r1), vld3q_u8(r2)};
    Wide sum{vdupq_n_u16(0), vdupq_n_u16(0)};
    for (const uint8x16x3_t& row : rows) {
        for (int k = 0; k < 3; ++k) {
            sum.lo = vaddw_u8(sum.lo, vget_low_u8(row.val[k]));
            sum.hi = vaddw_high_u8(sum.hi, row.val[k]);
        }
    }
    return Div9(sum);
}
#endif

/// g2-arm64: the decoder's 4:2:0 picture (src) into the NV12 frame's top-left width x height,
/// made smaller by an exact 3:2, 2:1 or 3:1 (1080p into 720p, 540p, 360p) with an area filter.
/// sws_scale's bilinear filter took 8 ms of the 16.7 a 60 FPS movie frame has for 720p on the
/// G2 (issue #6). False for another format, ratio or size: the caller scales with sws then.
bool AreaDownscale(const AVFrame& src, AVFrame& dst, s32 width, s32 height) {
#if defined(__aarch64__)
    if (src.format != AV_PIX_FMT_YUV420P) {
        return false;
    }
    const s32 w = src.width, h = src.height;
    const int ratio = width * 3 == w * 2 && height * 3 == h * 2 ? 32
                      : width * 2 == w && height * 2 == h       ? 21
                      : width * 3 == w && height * 3 == h       ? 31
                                                                : 0;
    const s32 block = ratio == 21 ? 32 : 48, rows = ratio == 21 ? 2 : 3;
    if (!ratio || w % (2 * block) || h % (2 * rows)) {
        return false; // whole blocks in the chroma planes too
    }
    const u8* const y_plane = src.data[0];
    const u8* const u_plane = src.data[1];
    const u8* const v_plane = src.data[2];
    const s32 ys = src.linesize[0], us = src.linesize[1], vs = src.linesize[2];
    u8* const luma = dst.data[0];
    u8* const chroma = dst.data[1];
    const s32 ls = dst.linesize[0], cs = dst.linesize[1];
    if (ratio == 32) {
        for (s32 y = 0; y < h; y += 3) {
            const u8* r = y_plane + y * ys;
            u8* d = luma + (y / 3 * 2) * ls;
            for (s32 x = 0; x < w; x += 48) {
                uint8x16x2_t top, bottom;
                Area32(r + x, r + ys + x, r + 2 * ys + x, top, bottom);
                vst2q_u8(d + x / 3 * 2, top);
                vst2q_u8(d + ls + x / 3 * 2, bottom);
            }
        }
        for (s32 y = 0; y < h / 2; y += 3) {
            const u8* u = u_plane + y * us;
            const u8* v = v_plane + y * vs;
            u8* d = chroma + (y / 3 * 2) * cs;
            for (s32 x = 0; x < w / 2; x += 48) {
                uint8x16x2_t ut, ub, vt, vb;
                Area32(u + x, u + us + x, u + 2 * us + x, ut, ub);
                Area32(v + x, v + vs + x, v + 2 * vs + x, vt, vb);
                vst4q_u8(d + x / 3 * 4,
                         (uint8x16x4_t{{ut.val[0], vt.val[0], ut.val[1], vt.val[1]}}));
                vst4q_u8(d + cs + x / 3 * 4,
                         (uint8x16x4_t{{ub.val[0], vb.val[0], ub.val[1], vb.val[1]}}));
            }
        }
    } else if (ratio == 21) {
        for (s32 y = 0; y < h; y += 2) {
            const u8* r = y_plane + y * ys;
            u8* d = luma + (y / 2) * ls;
            for (s32 x = 0; x < w; x += 32) {
                vst1q_u8(d + x / 2, Area21(r + x, r + ys + x));
            }
        }
        for (s32 y = 0; y < h / 2; y += 2) {
            const u8* u = u_plane + y * us;
            const u8* v = v_plane + y * vs;
            u8* d = chroma + (y / 2) * cs;
            for (s32 x = 0; x < w / 2; x += 32) {
                vst2q_u8(d + x,
                         (uint8x16x2_t{{Area21(u + x, u + us + x), Area21(v + x, v + vs + x)}}));
            }
        }
    } else {
        for (s32 y = 0; y < h; y += 3) {
            const u8* r = y_plane + y * ys;
            u8* d = luma + (y / 3) * ls;
            for (s32 x = 0; x < w; x += 48) {
                vst1q_u8(d + x / 3, Area31(r + x, r + ys + x, r + 2 * ys + x));
            }
        }
        for (s32 y = 0; y < h / 2; y += 3) {
            const u8* u = u_plane + y * us;
            const u8* v = v_plane + y * vs;
            u8* d = chroma + (y / 3) * cs;
            for (s32 x = 0; x < w / 2; x += 48) {
                vst2q_u8(d + x / 3 * 2,
                         (uint8x16x2_t{{Area31(u + x, u + us + x, u + 2 * us + x),
                                        Area31(v + x, v + vs + x, v + 2 * vs + x)}}));
            }
        }
    }
    return true;
#else
    return false;
#endif
}
} // namespace

void VdecDecoder::OutputSize(const AVFrame& frame, s32& width, s32& height, s32& content_width,
                             s32& content_height) {
    width = frame.width;
    height = frame.height;
    if (s32 w, h; EnvOutputSize(w, h)) {
        width = w;
        height = h;
    } else if (m_max_width > 0 && m_max_height > 0 &&
               (frame.width > m_max_width || frame.height > m_max_height)) {
        // Larger than the game configured the decoder for: its buffers are sized for that.
        width = m_max_width & ~1;
        height = m_max_height & ~1;
    }
    if (!EnvContentSize(content_width, content_height) || content_width > width ||
        content_height > height) {
        content_width = width;
        content_height = height;
    }
    if (!m_reported) {
        m_reported = true;
        std::printf("Videodec2: decoder configured for %dx%d; stream %dx%d, handed to the game as "
                    "%dx%d (picture %dx%d)\n",
                    m_max_width, m_max_height, frame.width, frame.height, width, height,
                    content_width, content_height);
    }
}

s32 VdecDecoder::Decode(const OrbisVideodec2InputData& input_data,
                        OrbisVideodec2FrameBuffer& frame_buffer,
                        OrbisVideodec2OutputInfo& output_info) {
    frame_buffer.is_accepted = false;
    output_info.is_valid = false;
    output_info.picture_count = 0;

    if (!input_data.au_data) {
        LOG_ERROR(Lib_Vdec2, "ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_POINTER");
        return ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_POINTER;
    }
    if (input_data.au_size == 0) {
        LOG_ERROR(Lib_Vdec2, "ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_SIZE");
        return ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_SIZE;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        LOG_ERROR(Lib_Vdec2, "Failed to allocate packet");
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    packet->data = (u8*)input_data.au_data;
    packet->size = input_data.au_size;
    packet->pts = input_data.pts_data;
    packet->dts = input_data.dts_data;
    packet->opaque = reinterpret_cast<void*>(input_data.attached_data);

    const u64 decode_start = NowNs();
    int ret = avcodec_send_packet(m_codec_context, packet);
    if (ret == AVERROR_EOF) {
        // Attempt to flush buffers and try again.
        avcodec_flush_buffers(m_codec_context);
        ret = avcodec_send_packet(m_codec_context, packet);
    }
    if (ret < 0) {
        LOG_ERROR(Lib_Vdec2, "Error sending packet to decoder: {}", ret);
        av_packet_free(&packet);
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    AVFrame* frame = av_frame_alloc();
    if (frame == nullptr) {
        LOG_ERROR(Lib_Vdec2, "Failed to allocate frame");
        av_packet_free(&packet);
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    ret = avcodec_receive_frame(m_codec_context, frame);
    AddDecodeNs(NowNs() - decode_start);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        LOG_TRACE(Lib_Vdec2, "AVERROR_EOF or AVERROR(EAGAIN)");
        av_packet_free(&packet);
        av_frame_free(&frame);
        return ORBIS_OK;
    } else if (ret < 0) {
        LOG_ERROR(Lib_Vdec2, "Error receiving frame from decoder: {}", ret);
        av_packet_free(&packet);
        av_frame_free(&frame);
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    if (frame->flags & AV_FRAME_FLAG_INTERLACED) {
        LOG_ERROR(Lib_Vdec2, "Interlaced video output is not suported.");
    }

    s32 out_width, out_height, content_width, content_height;
    OutputSize(*frame, out_width, out_height, content_width, content_height);
    const u64 convert_start = NowNs();
    if (frame->format != AV_PIX_FMT_NV12 || frame->width != content_width ||
        frame->height != content_height || content_width != out_width ||
        content_height != out_height) {
        AVFrame* nv12_frame =
            ConvertNV12Frame(*frame, out_width, out_height, content_width, content_height);
        ASSERT(nv12_frame);
        av_frame_free(&frame);
        frame = nv12_frame;
    }
    frame = FitToDisplay(frame);

    const bool is_avc = m_codec_context->codec_id == AV_CODEC_ID_H264;
    const u64 info_size =
        is_avc ? sizeof(OrbisVideodec2AvcPictureInfo) : sizeof(OrbisVideodec2HevcPictureInfo);
    const u64 copy_start = NowNs();
    Videodec::CopyNV12Data((u8*)frame_buffer.frame_buffer,
                           frame_buffer.frame_buffer_size - info_size, *frame, kHeightAlignment, PitchAlignment());
    g_convert_ns += copy_start - convert_start;
    g_copy_ns += NowNs() - copy_start;
    frame_buffer.is_accepted = true;

    const auto width = Common::AlignUp<u32>(frame->width, 16);
    const auto pitch = Common::AlignUp<u32>(frame->width, PitchAlignment());
    const auto height = Common::AlignUp<u32>(frame->height, kHeightAlignment);

    output_info.is_valid = true;
    output_info.is_error_frame = false;
    output_info.picture_count = 1; // TODO: 2 pictures for interlaced video
    output_info.codec_type = is_avc ? OrbisVideodec2CodecType::Avc : OrbisVideodec2CodecType::Hevc;
    output_info.frame_width = width;
    output_info.frame_pitch = pitch;
    output_info.frame_height = height;
    output_info.frame_buffer = frame_buffer.frame_buffer;
    output_info.frame_buffer_size = (pitch * height * 3) / 2;

    // Only set frameFormat and framePitchInBytes if the game uses the newer struct version.
    if (output_info.this_size == sizeof(OrbisVideodec2OutputInfo)) {
        output_info.frame_format = 0;
        output_info.frame_pitch_in_bytes = pitch;
    }

    if (is_avc) {
        auto& picture_info = *(OrbisVideodec2AvcPictureInfo*)((u8*)output_info.frame_buffer +
                                                              output_info.frame_buffer_size);

        picture_info = {};
        picture_info.is_valid = true;

        picture_info.pts_data = frame->pts;
        picture_info.dts_data = frame->pkt_dts;
        picture_info.attached_data = reinterpret_cast<u64>(frame->opaque);

        picture_info.frame_crop_top_offset = 0;
        picture_info.frame_crop_left_offset = 0;
        picture_info.frame_crop_right_offset = pitch - frame->width;
        picture_info.frame_crop_bottom_offset = height - frame->height;
    } else {
        auto& picture_info = *(OrbisVideodec2HevcPictureInfo*)((u8*)output_info.frame_buffer +
                                                               output_info.frame_buffer_size);
        picture_info = {};
        picture_info.is_valid = true;

        picture_info.pts_data = frame->pts;
        picture_info.dts_data = frame->pkt_dts;
        picture_info.attached_data = reinterpret_cast<u64>(frame->opaque);

        picture_info.frame_crop_top_offset = 0;
        picture_info.frame_crop_left_offset = 0;
        picture_info.frame_crop_right_offset = pitch - frame->width;
        picture_info.frame_crop_bottom_offset = height - frame->height;
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    return ORBIS_OK;
}

s32 VdecDecoder::Flush(OrbisVideodec2FrameBuffer& frame_buffer,
                       OrbisVideodec2OutputInfo& output_info) {
    frame_buffer.is_accepted = false;
    output_info.is_valid = false;
    output_info.picture_count = 0;

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        LOG_ERROR(Lib_Vdec2, "Failed to allocate frame");
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    avcodec_send_packet(m_codec_context, nullptr);
    int ret = avcodec_receive_frame(m_codec_context, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        av_frame_free(&frame);
        return ORBIS_OK;
    } else if (ret < 0) {
        LOG_ERROR(Lib_Vdec2, "Error receiving frame from decoder: {}", ret);
        av_frame_free(&frame);
        return ORBIS_VIDEODEC2_ERROR_API_FAIL;
    }

    s32 out_width, out_height, content_width, content_height;
    OutputSize(*frame, out_width, out_height, content_width, content_height);
    if (frame->format != AV_PIX_FMT_NV12 || frame->width != content_width ||
        frame->height != content_height || content_width != out_width ||
        content_height != out_height) {
        AVFrame* nv12_frame =
            ConvertNV12Frame(*frame, out_width, out_height, content_width, content_height);
        ASSERT(nv12_frame);
        av_frame_free(&frame);
        frame = nv12_frame;
    }
    frame = FitToDisplay(frame);

    const bool is_avc = m_codec_context->codec_id == AV_CODEC_ID_H264;
    const u64 info_size =
        is_avc ? sizeof(OrbisVideodec2AvcPictureInfo) : sizeof(OrbisVideodec2HevcPictureInfo);
    Videodec::CopyNV12Data((u8*)frame_buffer.frame_buffer,
                           frame_buffer.frame_buffer_size - info_size, *frame, kHeightAlignment, PitchAlignment());
    frame_buffer.is_accepted = true;

    const auto width = Common::AlignUp<u32>(frame->width, 16);
    const auto pitch = Common::AlignUp<u32>(frame->width, PitchAlignment());
    const auto height = Common::AlignUp<u32>(frame->height, kHeightAlignment);

    output_info.is_valid = true;
    output_info.is_error_frame = false;
    output_info.picture_count = 1; // TODO: 2 pictures for interlaced video
    output_info.codec_type = is_avc ? OrbisVideodec2CodecType::Avc : OrbisVideodec2CodecType::Hevc;
    output_info.frame_width = width;
    output_info.frame_pitch = pitch;
    output_info.frame_height = height;
    output_info.frame_buffer = frame_buffer.frame_buffer;
    output_info.frame_buffer_size = (pitch * height * 3) / 2;

    // Only set frameFormat and framePitchInBytes if the game uses the newer struct version.
    if (output_info.this_size == sizeof(OrbisVideodec2OutputInfo)) {
        output_info.frame_format = 0;
        output_info.frame_pitch_in_bytes = pitch;
    }

    if (m_codec_context->codec_id == AV_CODEC_ID_H264) {
        auto& picture_info = *(OrbisVideodec2AvcPictureInfo*)((u8*)output_info.frame_buffer +
                                                              output_info.frame_buffer_size);

        picture_info = {};
        picture_info.is_valid = true;

        picture_info.pts_data = frame->pts;
        picture_info.dts_data = frame->pkt_dts;
        picture_info.attached_data = reinterpret_cast<u64>(frame->opaque);

        picture_info.frame_crop_top_offset = 0;
        picture_info.frame_crop_left_offset = 0;
        picture_info.frame_crop_right_offset = pitch - frame->width;
        picture_info.frame_crop_bottom_offset = height - frame->height;
    } else {
        auto& picture_info = *(OrbisVideodec2HevcPictureInfo*)((u8*)output_info.frame_buffer +
                                                               output_info.frame_buffer_size);
        picture_info = {};
        picture_info.is_valid = true;

        picture_info.pts_data = frame->pts;
        picture_info.dts_data = frame->pkt_dts;
        picture_info.attached_data = reinterpret_cast<u64>(frame->opaque);

        picture_info.frame_crop_top_offset = 0;
        picture_info.frame_crop_left_offset = 0;
        picture_info.frame_crop_right_offset = pitch - frame->width;
        picture_info.frame_crop_bottom_offset = height - frame->height;
    }

    av_frame_free(&frame);
    return ORBIS_OK;
}

s32 VdecDecoder::Reset() {
    avcodec_flush_buffers(m_codec_context);
    return ORBIS_OK;
}

AVFrame* VdecDecoder::ConvertNV12Frame(AVFrame& frame, s32 width, s32 height,
                                       s32 content_width, s32 content_height) {
    // g2-arm64: one NV12 frame for the decoder's life, handed out as a new reference each time;
    // the caller has freed the previous one, so the buffer is free to write again.
    if (!m_nv12_frame || m_nv12_frame->width != width || m_nv12_frame->height != height) {
        av_frame_free(&m_nv12_frame);
        m_nv12_frame = av_frame_alloc();
        m_nv12_frame->format = AV_PIX_FMT_NV12;
        m_nv12_frame->width = width;
        m_nv12_frame->height = height;
        if (av_frame_get_buffer(m_nv12_frame, 0) < 0) {
            av_frame_free(&m_nv12_frame);
            return nullptr;
        }
        m_nv12_content_width = m_nv12_content_height = 0;
    }
    AVFrame* nv12_frame = m_nv12_frame;
    nv12_frame->pts = frame.pts;
    nv12_frame->pkt_dts = frame.pkt_dts < 0 ? 0 : frame.pkt_dts;
    nv12_frame->sample_aspect_ratio = frame.sample_aspect_ratio;
    nv12_frame->crop_top = frame.crop_top;
    nv12_frame->crop_bottom = frame.crop_bottom;
    nv12_frame->crop_left = frame.crop_left;
    nv12_frame->crop_right = frame.crop_right;
    nv12_frame->opaque = frame.opaque;

    if ((content_width != width || content_height != height) &&
        (content_width != m_nv12_content_width || content_height != m_nv12_content_height)) {
        // Black around the picture (limited range: Y 16, Cb/Cr 128), once: the picture is
        // written into the same place every frame.
        for (s32 y = 0; y < height; ++y) {
            std::memset(nv12_frame->data[0] + y * nv12_frame->linesize[0], 16, width);
        }
        for (s32 y = 0; y < height / 2; ++y) {
            std::memset(nv12_frame->data[1] + y * nv12_frame->linesize[1], 128, width);
        }
        m_nv12_content_width = content_width;
        m_nv12_content_height = content_height;
    }

    int res = 0;
    if (!AreaDownscale(frame, *nv12_frame, content_width, content_height)) {
        m_sws_context = sws_getCachedContext(m_sws_context, frame.width, frame.height,
                                             AVPixelFormat(frame.format), content_width,
                                             content_height, AV_PIX_FMT_NV12, SWS_BILINEAR,
                                             nullptr, nullptr, nullptr);
        res = sws_scale(m_sws_context, frame.data, frame.linesize, 0, frame.height,
                        nv12_frame->data, nv12_frame->linesize);
    }
    if (res < 0) {
        LOG_ERROR(Lib_Vdec2, "Could not convert to NV12: {}", av_err2str(res));
        return nullptr;
    }

    AVFrame* reference = av_frame_alloc();
    if (!reference || av_frame_ref(reference, nv12_frame) < 0) {
        av_frame_free(&reference);
        return nullptr;
    }
    return reference;
}

void GameResolution(s32& width, s32& height) {
    static s32 w = -1, h = 0;
    if (w < 0) {
        w = 0;
        unsigned pw = 0, ph = 0;
        if (const char* res = std::getenv("BB_GAME_RES");
            res && std::sscanf(res, "%ux%u", &pw, &ph) == 2 && pw >= 640 && ph >= 360 &&
            !(pw == 1920 && ph == 1080)) {
            w = static_cast<s32>(pw & ~1u);
            h = static_cast<s32>(ph & ~1u);
        }
    }
    width = w;
    height = h;
}

AVFrame* VdecDecoder::FitToDisplay(AVFrame* frame) {
    s32 target_w = 0, target_h = 0;
    GameResolution(target_w, target_h);
    if (!target_w || !frame || frame->format != AV_PIX_FMT_NV12 ||
        (frame->width == target_w && frame->height == target_h)) {
        return frame;
    }
    AVFrame* canvas = av_frame_alloc();
    canvas->format = AV_PIX_FMT_NV12;
    canvas->width = target_w;
    canvas->height = target_h;
    canvas->pts = frame->pts;
    canvas->pkt_dts = frame->pkt_dts < 0 ? 0 : frame->pkt_dts;
    canvas->opaque = frame->opaque;
    if (av_frame_get_buffer(canvas, 0) < 0) {
        av_frame_free(&canvas);
        return frame;
    }
    for (s32 y = 0; y < target_h; ++y) {
        std::memset(canvas->data[0] + y * canvas->linesize[0], 16, target_w);
    }
    for (s32 y = 0; y < target_h / 2; ++y) {
        std::memset(canvas->data[1] + y * canvas->linesize[1], 128, target_w);
    }
    // Fit inside the canvas keeping the movie's shape (even sizes: 4:2:0).
    const double scale = std::min(static_cast<double>(target_w) / frame->width,
                                  static_cast<double>(target_h) / frame->height);
    s32 fit_w = std::min(target_w, static_cast<s32>(frame->width * scale + 0.5) & ~1);
    s32 fit_h = std::min(target_h, static_cast<s32>(frame->height * scale + 0.5) & ~1);
    const s32 x0 = ((target_w - fit_w) / 2) & ~1, y0 = ((target_h - fit_h) / 2) & ~1;
    m_fit_context = sws_getCachedContext(m_fit_context, frame->width, frame->height, AV_PIX_FMT_NV12,
                                         fit_w, fit_h, AV_PIX_FMT_NV12, SWS_BICUBIC, nullptr,
                                         nullptr, nullptr);
    if (m_fit_context) {
        u8* dst[4] = {canvas->data[0] + y0 * canvas->linesize[0] + x0,
                      canvas->data[1] + (y0 / 2) * canvas->linesize[1] + x0, nullptr, nullptr};
        const int dst_stride[4] = {canvas->linesize[0], canvas->linesize[1], 0, 0};
        sws_scale(m_fit_context, frame->data, frame->linesize, 0, frame->height, dst, dst_stride);
    }
    av_frame_free(&frame);
    return canvas;
}

} // namespace Libraries::Videodec2
