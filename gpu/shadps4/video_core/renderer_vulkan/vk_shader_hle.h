// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace AmdGpu {
struct ComputeProgram;
union Regs;
} // namespace AmdGpu

namespace Shader {
struct Info;
}

namespace Vulkan {

class Rasterizer;

/// bbport: shaders whose HLE writes guest memory. They run where the dispatch is processed,
/// not on the draw recording thread: the SRT walkers of the draws after them read that memory.
bool IsGuestMemoryShaderHLE(unsigned long long pgm_hash);

/// bbport: whether a guest memory HLE shader (IsGuestMemoryShaderHLE) can go down the draw pipe
/// and run on the recording thread in order, instead of the GPU thread draining the pipe first.
bool PipelineGuestMemoryShaderHLE(const Shader::Info& info,
                                  const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer);

/// g2-arm64: a guest memory scalar copy (PipelineGuestMemoryShaderHLE false) made in guest
/// memory here and now, on the GPU command thread without a drain, when no packet handed over
/// still reads or writes its ranges. True: made; the dispatch then goes down the pipe flagged.
bool ScalarCopyAheadHLE(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                        Rasterizer& rasterizer);

/// Attempts to execute a shader using HLE if possible.
bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer);

} // namespace Vulkan
