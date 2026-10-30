/* g2-arm64: the loader's input files from a game folder (the user's own plaintext dump), byte for
 * byte as scripts/prepare.py + link_modules.py + content_profile.py make them on a PC: the
 * Android app has no Python. No decryption: encrypted or compressed SELF segments are refused,
 * as there. The Python scripts stay the reference; this follows them step by step. */
#include "game_prepare.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_IMAGE (512ull * 1024 * 1024)

typedef struct {
    jmp_buf fail;
    char *error;
    size_t error_size;
    void **blocks;
    size_t nblocks, capacity;
} Ctx;

static void fail(Ctx *c, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vsnprintf(c->error, c->error_size, format, ap);
    va_end(ap);
    longjmp(c->fail, 1);
}

/* Every allocation is owned by the context and freed at the end, after a failure too. */
static void *track(Ctx *c, void *p) {
    if (!p) fail(c, "out of memory");
    if (c->nblocks == c->capacity) {
        size_t capacity = c->capacity ? c->capacity * 2 : 64;
        void **blocks = realloc(c->blocks, capacity * sizeof(*blocks));
        if (!blocks) { free(p); fail(c, "out of memory"); }
        c->blocks = blocks;
        c->capacity = capacity;
    }
    c->blocks[c->nblocks++] = p;
    return p;
}
static void *zalloc(Ctx *c, size_t n) { return track(c, calloc(n ? n : 1, 1)); }
static void *grow(Ctx *c, void *p, size_t n) {
    for (size_t i = c->nblocks; i-- > 0;)
        if (c->blocks[i] == p) {
            void *q = realloc(p, n ? n : 1);
            if (!q) fail(c, "out of memory");
            c->blocks[i] = q;
            return q;
        }
    return track(c, realloc(p, n ? n : 1));
}
static char *dup_string(Ctx *c, const char *s) {
    size_t n = strlen(s) + 1;
    return memcpy(zalloc(c, n), s, n);
}

typedef struct { uint8_t *data; uint64_t size; } Buf;

/* g2-arm64 (issue #10): game_dir may be "<update folder>\n<game folder>": a file is taken from the
 * first folder that has it. The game folder's path for messages and single-folder uses. */
static const char *game_root(const char *game_dir) {
    const char *nl = strchr(game_dir, '\n');
    return nl ? nl + 1 : game_dir;
}
/* <game_dir>/<name> from the update folder when it has the file, from the game's folder
 * otherwise (game_dir as game_root says). */
static void game_file(char *path, size_t size, const char *game_dir, const char *name) {
    const char *nl = strchr(game_dir, '\n');
    if (nl) {
        snprintf(path, size, "%.*s/%s", (int)(nl - game_dir), game_dir, name);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); return; }
    }
    snprintf(path, size, "%s/%s", game_root(game_dir), name);
}

static Buf read_file(Ctx *c, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) fail(c, "cannot open %s", path);
    Buf b = {0, 0};
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if (n >= 0 && fseek(f, 0, SEEK_SET) == 0) {
            b.size = (uint64_t)n;
            b.data = zalloc(c, (size_t)b.size);
            if (fread(b.data, 1, (size_t)b.size, f) != (size_t)b.size) b.data = NULL;
        }
    }
    fclose(f);
    if (!b.data) fail(c, "cannot read %s", path);
    return b;
}

static const uint8_t *span(Ctx *c, const uint8_t *data, uint64_t length, uint64_t offset, uint64_t size) {
    if (offset > length || size > length - offset)
        fail(c, "out of bounds: %#llx+%#llx / %#llx", (unsigned long long)offset, (unsigned long long)size,
             (unsigned long long)length);
    return data + offset;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i)); }

/* A growable byte string for the output file. */
typedef struct { uint8_t *data; size_t size, capacity; } Out;
static void put(Ctx *c, Out *o, const void *p, size_t n) {
    if (o->size + n > o->capacity) {
        size_t capacity = o->capacity ? o->capacity : 4096;
        while (capacity < o->size + n) capacity *= 2;
        o->data = grow(c, o->data, capacity);
        o->capacity = capacity;
    }
    memcpy(o->data + o->size, p, n);
    o->size += n;
}
static void put64(Ctx *c, Out *o, uint64_t v) { uint8_t b[8]; wr64(b, v); put(c, o, b, 8); }
static void put32(Ctx *c, Out *o, uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; put(c, o, b, 4); }

/* ---- a string-keyed map (open addressing) ---- */
typedef struct { char **keys; int64_t *values; size_t capacity, count; } Map;
static uint64_t hash_string(const char *s) {
    uint64_t h = 1469598103934665603ull;
    while (*s) h = (h ^ (uint8_t)*s++) * 1099511628211ull;
    return h;
}
static int64_t *map_slot(Ctx *c, Map *m, const char *key, int insert) {
    if (insert && (m->count + 1) * 2 > m->capacity) {
        Map bigger = {0, 0, m->capacity ? m->capacity * 2 : 256, 0};
        bigger.keys = zalloc(c, bigger.capacity * sizeof(char *));
        bigger.values = zalloc(c, bigger.capacity * sizeof(int64_t));
        for (size_t i = 0; i < m->capacity; ++i)
            if (m->keys[i]) *map_slot(c, &bigger, m->keys[i], 1) = m->values[i];
        *m = bigger;
    }
    if (!m->capacity) return NULL;
    for (size_t i = hash_string(key) & (m->capacity - 1);; i = (i + 1) & (m->capacity - 1)) {
        if (!m->keys[i]) {
            if (!insert) return NULL;
            m->keys[i] = dup_string(c, key);
            ++m->count;
            return &m->values[i];
        }
        if (!strcmp(m->keys[i], key)) return &m->values[i];
    }
}

/* ---- SELF -> ELF (prepare.py parse_self) ---- */
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Phdr;

typedef struct { const char *name; unsigned version; } Ident; /* a library or module */
typedef struct { const char *nid; Ident library, module; int valid; char key[192]; } Identity;
typedef struct { const char *name; unsigned type, binding, section; uint64_t value, size; Identity identity; } Symbol;
typedef struct { uint64_t target; uint32_t kind, symbol; int64_t addend; } Reloc;

typedef struct {
    Buf elf;
    uint64_t entry;
    Phdr ph[256];
    int nph;
    uint64_t (*dyn)[2];
    int ndyn;
    const uint8_t *blob;
    uint64_t blob_size;
    const uint8_t *strings;
    uint64_t strings_size;
    Symbol *symbols;
    size_t nsymbols;
    Reloc *relocs;
    size_t nrelocs;
    Map libraries, modules; /* code -> index into ident tables */
    Ident *idents;
    size_t nidents;
} Module;

static void parse_self(Ctx *c, const Buf *src, Module *m) {
    const uint8_t *d = src->data;
    uint64_t n = src->size;
    if (memcmp(span(c, d, n, 0, 4), "O\x15=\x1d", 4)) fail(c, "expected PS4 SELF");
    unsigned count = rd16(span(c, d, n, 24, 2));
    uint64_t base = 32 + (uint64_t)count * 32;
    const uint8_t *h = span(c, d, n, base, 64);
    if (memcmp(h, "\x7f" "ELF\x02\x01\x01", 7) || rd16(h + 18) != 62) fail(c, "expected little-endian x86-64 ELF");
    m->entry = rd64(h + 24);
    uint64_t phoff = rd64(h + 32);
    unsigned phentsize = rd16(h + 54), phnum = rd16(h + 56);
    if (phentsize != 56 || !(0 < phnum && phnum < 256)) fail(c, "unsupported program headers");
    m->nph = (int)phnum;
    uint64_t end = 0;
    for (unsigned i = 0; i < phnum; ++i) {
        const uint8_t *p = span(c, d, n, base + phoff + i * 56ull, 56);
        Phdr *q = &m->ph[i];
        q->type = rd32(p); q->flags = rd32(p + 4); q->offset = rd64(p + 8); q->vaddr = rd64(p + 16);
        q->paddr = rd64(p + 24); q->filesz = rd64(p + 32); q->memsz = rd64(p + 40); q->align = rd64(p + 48);
        if (q->offset + q->filesz > end) end = q->offset + q->filesz;
    }
    if (end > MAX_IMAGE) fail(c, "probe image exceeds 512 MiB limit");
    uint64_t header_end = phoff + phnum * 56ull;
    m->elf.size = end > header_end ? end : header_end;
    m->elf.data = zalloc(c, (size_t)m->elf.size);
    memcpy(m->elf.data, span(c, d, n, base, header_end), (size_t)header_end);
    uint64_t (*covered)[2] = zalloc(c, (count + 1) * sizeof(*covered));
    size_t ncovered = 0;
    covered[ncovered][0] = 0; covered[ncovered++][1] = header_end;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *s = span(c, d, n, 32 + i * 32ull, 32);
        uint64_t flags = rd64(s), off = rd64(s + 8), size = rd64(s + 16), mem = rd64(s + 24);
        if (!(flags & 0x800)) continue;
        if (flags & 10) fail(c, "encrypted/compressed SELF segment is unsupported");
        unsigned index = (unsigned)(flags >> 20) & 4095;
        if (index >= phnum) fail(c, "invalid segment index");
        const Phdr *p = &m->ph[index];
        if (size != p->filesz || size != mem) fail(c, "unsupported blocked segment layout");
        memcpy(m->elf.data + p->offset, span(c, d, n, off, size), (size_t)size);
        covered[ncovered][0] = p->offset; covered[ncovered++][1] = p->offset + size;
    }
    for (int i = 0; i < m->nph; ++i) {
        const Phdr *p = &m->ph[i];
        if (!p->filesz) continue;
        int inside = 0;
        for (size_t k = 0; k < ncovered && !inside; ++k)
            inside = covered[k][0] <= p->offset && p->offset + p->filesz <= covered[k][1];
        if (!inside && (p->type == 1 || p->type == 2 || p->type == 0x61000000 || p->type == 0x61000010))
            fail(c, "required segment %d is unavailable", i);
    }
}

static const Phdr *first_ph(Ctx *c, const Module *m, uint32_t type) {
    for (int i = 0; i < m->nph; ++i)
        if (m->ph[i].type == type) return &m->ph[i];
    fail(c, "no program header of type %#x", type);
    return NULL;
}

static int has_tag(const Module *m, uint64_t tag, uint64_t *value) {
    for (int i = m->ndyn; i-- > 0;) /* dict(dynamic): the last one wins */
        if (m->dyn[i][0] == tag) { if (value) *value = m->dyn[i][1]; return 1; }
    return 0;
}
static uint64_t tag_of(Ctx *c, const Module *m, uint64_t tag) {
    uint64_t v;
    if (!has_tag(m, tag, &v)) fail(c, "missing dynamic tag %#llx", (unsigned long long)tag);
    return v;
}

static const char *string_at(Ctx *c, const Module *m, uint64_t offset) {
    span(c, m->strings, m->strings_size, offset, 1);
    const uint8_t *s = m->strings + offset, *nul = memchr(s, 0, (size_t)(m->strings_size - offset));
    if (!nul) fail(c, "unterminated string");
    for (const uint8_t *p = s; p < nul; ++p)
        if (*p >= 0x80) fail(c, "non-ASCII symbol string");
    return (const char *)s;
}

static const char *encode_id(Ctx *c, uint64_t value) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    char buf[16];
    int at = sizeof(buf) - 1;
    buf[at] = 0;
    buf[--at] = alphabet[value & 63];
    value >>= 6;
    while (value) { buf[--at] = alphabet[value & 63]; value >>= 6; }
    return dup_string(c, buf + at);
}

static Identity make_identity(Ctx *c, const Module *m, const char *name) {
    Identity id;
    memset(&id, 0, sizeof(id));
    const char *h1 = strchr(name, '#'), *h2 = h1 ? strchr(h1 + 1, '#') : NULL;
    if (!h1 || !h2 || strchr(h2 + 1, '#')) return id; /* plain names such as module_start */
    char nid[64], lib[64], mod[64];
    size_t l0 = (size_t)(h1 - name), l1 = (size_t)(h2 - h1 - 1), l2 = strlen(h2 + 1);
    if (l0 >= sizeof(nid) || l1 >= sizeof(lib) || l2 >= sizeof(mod)) fail(c, "symbol name too long: %s", name);
    memcpy(nid, name, l0); nid[l0] = 0;
    memcpy(lib, h1 + 1, l1); lib[l1] = 0;
    memcpy(mod, h2 + 1, l2); mod[l2] = 0;
    int64_t *li = map_slot(c, (Map *)&m->libraries, lib, 0), *mi = map_slot(c, (Map *)&m->modules, mod, 0);
    if (!li || !mi) fail(c, "unknown library or module code in %s", name);
    id.nid = dup_string(c, nid);
    id.library = m->idents[*li];
    id.module = m->idents[*mi];
    id.valid = 1;
    snprintf(id.key, sizeof(id.key), "%s\x1f%s\x1f%u\x1f%s\x1f%u", id.nid, id.library.name, id.library.version,
             id.module.name, id.module.version);
    return id;
}

/* link_libc.py/link_modules.py module(): the ELF, its dynamic section, symbols, relocations and
 * library/module tables. */
static void load_module(Ctx *c, const char *path, Module *m) {
    memset(m, 0, sizeof(*m));
    Buf src = read_file(c, path);
    parse_self(c, &src, m);
    const Phdr *dp = first_ph(c, m, 2);
    m->dyn = zalloc(c, (size_t)(dp->filesz / 16 + 1) * sizeof(*m->dyn));
    for (uint64_t pos = dp->offset; pos < dp->offset + dp->filesz; pos += 16) {
        const uint8_t *e = span(c, m->elf.data, m->elf.size, pos, 16);
        if (!rd64(e)) break;
        m->dyn[m->ndyn][0] = rd64(e);
        m->dyn[m->ndyn++][1] = rd64(e + 8);
    }
    const Phdr *lp = first_ph(c, m, 0x61000000);
    m->blob = span(c, m->elf.data, m->elf.size, lp->offset, lp->filesz);
    m->blob_size = lp->filesz;
    m->strings_size = tag_of(c, m, 0x61000037);
    m->strings = span(c, m->blob, m->blob_size, tag_of(c, m, 0x61000035), m->strings_size);
    m->idents = zalloc(c, (size_t)(m->ndyn + 1) * sizeof(*m->idents));
    for (int i = 0; i < m->ndyn; ++i) {
        uint64_t tag = m->dyn[i][0], value = m->dyn[i][1];
        if (tag != 0x61000013 && tag != 0x61000015 && tag != 0x6100000d && tag != 0x6100000f) continue;
        Map *table = tag == 0x61000013 || tag == 0x61000015 ? &m->libraries : &m->modules;
        const char *key = encode_id(c, value >> 48);
        Ident ident = {string_at(c, m, value & 0xffffffff), (unsigned)(value >> 32) & 65535};
        int64_t *slot = map_slot(c, table, key, 0);
        if (slot) {
            const Ident *old = &m->idents[*slot];
            if (strcmp(old->name, ident.name) || old->version != ident.version) fail(c, "conflicting module/library IDs");
        } else {
            m->idents[m->nidents] = ident;
            *map_slot(c, table, key, 1) = (int64_t)m->nidents++;
        }
    }
    uint64_t symtab_size = tag_of(c, m, 0x6100003f);
    const uint8_t *table = span(c, m->blob, m->blob_size, tag_of(c, m, 0x61000039), symtab_size);
    m->nsymbols = (size_t)(symtab_size / 24);
    if (symtab_size % 24) fail(c, "bad symbol table size");
    m->symbols = zalloc(c, (m->nsymbols + 1) * sizeof(Symbol));
    for (size_t i = 0; i < m->nsymbols; ++i) {
        const uint8_t *e = table + i * 24;
        Symbol *s = &m->symbols[i];
        s->name = string_at(c, m, rd32(e));
        s->type = e[4] & 15;
        s->binding = e[4] >> 4;
        s->section = rd16(e + 6);
        s->value = rd64(e + 8);
        s->size = rd64(e + 16);
        if (*s->name) s->identity = make_identity(c, m, s->name);
    }
    static const uint64_t tables[2][2] = {{0x61000029, 0x6100002d}, {0x6100002f, 0x61000031}};
    size_t total = 0;
    for (int t = 0; t < 2; ++t) total += (size_t)(tag_of(c, m, tables[t][1]) / 24);
    m->relocs = zalloc(c, (total + 1) * sizeof(Reloc));
    for (int t = 0; t < 2; ++t) {
        uint64_t size = tag_of(c, m, tables[t][1]);
        const uint8_t *r = span(c, m->blob, m->blob_size, tag_of(c, m, tables[t][0]), size);
        if (size % 24) fail(c, "bad relocation table size");
        for (uint64_t pos = 0; pos < size; pos += 24) {
            Reloc *q = &m->relocs[m->nrelocs++];
            q->target = rd64(r + pos);
            uint64_t info = rd64(r + pos + 8);
            q->kind = (uint32_t)info;
            q->symbol = (uint32_t)(info >> 32);
            q->addend = (int64_t)rd64(r + pos + 16);
        }
    }
}

static int is_load(const Phdr *p) { return p->type == 1 || p->type == 0x61000010; }
static int mapped(const Module *m, uint64_t address, uint64_t size) {
    for (int i = 0; i < m->nph; ++i) {
        const Phdr *p = &m->ph[i];
        if (is_load(p) && p->vaddr <= address && address + size <= p->vaddr + p->memsz) return 1;
    }
    return 0;
}

/* prepare.py inspect_libc: whether this dump's _init_env is exactly RET. */
static int init_env_is_ret(Ctx *c, const Module *libc) {
    int ret = 0;
    for (size_t i = 0; i < libc->nsymbols; ++i) {
        const Symbol *s = &libc->symbols[i];
        const char *hash = strchr(s->name, '#');
        size_t keylen = hash ? (size_t)(hash - s->name) : strlen(s->name);
        int init_env = keylen == 11 && !strncmp(s->name, "bzQExy189ZI", 11);       /* nid('_init_env') */
        int ios_init = keylen == 11 && !strncmp(s->name, "sqWytnhYdEg", 11);       /* nid('_ZNSt8ios_base4InitC1Ev') */
        if (!(init_env || ios_init) || !s->section || s->type != 2) continue;
        const Phdr *p = NULL;
        for (int k = 0; k < libc->nph && !p; ++k) {
            const Phdr *q = &libc->ph[k];
            if (q->type == 1 && (q->flags & 1) && q->vaddr <= s->value && s->value < q->vaddr + q->filesz) p = q;
        }
        if (!p) fail(c, "libc symbol outside its code");
        const uint8_t *code = span(c, libc->elf.data, libc->elf.size, p->offset + s->value - p->vaddr, s->size);
        if (init_env) ret = s->size == 1 && code[0] == 0xc3;
    }
    return ret;
}

/* link_modules.py: the host runtime's spelling of an import (the executable it was first
 * written for), by library name, otherwise NID#library. */
static const struct { const char *library, *codes[2]; } spelling[] = {
    {"libSceAjm", {"N", "O"}}, {"libSceAppContent", {"c", "d"}}, {"libSceAudioIn", {"Y", "Z"}},
    {"libSceAudioOut", {"G", "H"}}, {"libSceAvPlayer", {"m", "n"}}, {"libSceCommonDialog", {"Z", "a"}},
    {"libSceFios2", {"F", "G"}}, {"libSceGameLiveStreaming", {"e", "f"}}, {"libSceGnmDriver", {"B", "C"}},
    {"libSceHttp", {"K", "L"}}, {"libSceImeDialog", {"b", "c"}}, {"libSceMouse", {"U", "V"}},
    {"libSceMsgDialog", {"j", "k"}}, {"libSceNet", {"E", "F"}}, {"libSceNetCtl", {"J", "K"}},
    {"libSceNpAuth", {"X", "Y"}}, {"libSceNpCommerce", {"k", "l"}}, {"libSceNpCommon", {"Q", "R"}},
    {"libSceNpManager", {"R", "S"}}, {"libSceNpMatching2", {"P", "Q"}}, {"libSceNpProfileDialog", {"h", "i"}},
    {"libSceNpScore", {"o", "p"}}, {"libSceNpSignaling", {"T", "U"}}, {"libSceNpTrophy", {"l", "m"}},
    {"libSceNpUtility", {"S", "T"}}, {"libSceNpWebApi", {"W", "X"}}, {"libScePad", {"A", "B"}},
    {"libScePlayGo", {"n", "o"}}, {"libScePosix", {"I", "J"}}, {"libSceRtc", {"H", "I"}},
    {"libSceSaveData", {"O", "P"}}, {"libSceSaveDataDialog", {"d", "e"}}, {"libSceScreenShot", {"g", "h"}},
    {"libSceSsl", {"L", "M"}}, {"libSceSysmodule", {"M", "N"}}, {"libSceSystemService", {"V", "W"}},
    {"libSceUserService", {"D", "E"}}, {"libSceVideoOut", {"C", "D"}}, {"libSceVideoRecording", {"f", "g"}},
    {"libSceVoice", {"i", "j"}}, {"libSceWebBrowserDialog", {"a", "b"}}, {"libc", {"q", "q"}},
    {"libkernel", {"p", "J"}},
};

static const char *runtime_name(Ctx *c, const Identity *id) {
    char buf[256];
    const char *module_of = id->library.name;
    if (!strcmp(id->library.name, "libSceAppContent")) module_of = "libSceAppContentUtil";
    else if (!strcmp(id->library.name, "libScePosix")) module_of = "libkernel";
    for (size_t i = 0; i < sizeof(spelling) / sizeof(spelling[0]); ++i)
        if (!strcmp(spelling[i].library, id->library.name)) {
            if (!strcmp(id->module.name, module_of)) {
                snprintf(buf, sizeof(buf), "%s#%s#%s", id->nid, spelling[i].codes[0], spelling[i].codes[1]);
                return dup_string(c, buf);
            }
            break;
        }
    snprintf(buf, sizeof(buf), "%s#%s", id->nid, id->library.name);
    return dup_string(c, buf);
}

/* Rewrite initial-exec `mov rax, fs:[0]` to GS in the executable segments: glibc owns FS on
 * Linux, the runtime points GS at each guest thread's TCB. */
static void patch_fs_loads(const Module *m, uint8_t *image, uint64_t image_size, uint64_t base) {
    static const uint8_t fs_load[9] = {0x64, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};
    for (int i = 0; i < m->nph; ++i) {
        const Phdr *p = &m->ph[i];
        if (p->type != 1 || !(p->flags & 1)) continue;
        uint64_t start = base + p->vaddr, end = base + p->vaddr + p->filesz;
        if (end > image_size) end = image_size;
        for (uint64_t at = start; at + sizeof(fs_load) <= end;) {
            const uint8_t *hit = memchr(image + at, 0x64, (size_t)(end - at));
            if (!hit) break;
            at = (uint64_t)(hit - image);
            if (at + sizeof(fs_load) > end) break;
            if (!memcmp(hit, fs_load, sizeof(fs_load))) { image[at] = 0x65; at += sizeof(fs_load); }
            else ++at;
        }
    }
}

typedef struct { uint64_t target, kind; int64_t value, addend; } OutReloc; /* '<QQqq' */
typedef struct { uint64_t vaddr, memsz, flags; } Segment;
typedef struct { uint64_t address, kind; const char *file; } Export;

static void write_file(Ctx *c, const char *dir, const char *name, const void *data, size_t size) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    char part[4200];
    snprintf(part, sizeof(part), "%s.part", path);
    FILE *f = fopen(part, "wb");
    if (!f) fail(c, "cannot write %s", part);
    int ok = fwrite(data, 1, size, f) == size;
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(part); fail(c, "cannot write %s", part); }
    remove(path);
    if (rename(part, path)) fail(c, "cannot rename %s", part);
}

static void prepare_and_link(Ctx *c, const char *game_dir, const char *exe, const char *out_dir) {
    char path[4096];
    /* prepare.py: the executable's memory image, its relocations and imports. */
    Module main;
    game_file(path, sizeof(path), game_dir, exe);
    load_module(c, path, &main);
    uint64_t size = 0;
    size_t nloads = 0;
    for (int i = 0; i < main.nph; ++i)
        if (is_load(&main.ph[i])) {
            ++nloads;
            if (main.ph[i].vaddr + main.ph[i].memsz > size) size = main.ph[i].vaddr + main.ph[i].memsz;
        }
    if (size > MAX_IMAGE || !mapped(&main, main.entry, 1)) fail(c, "invalid memory image");
    uint64_t image_size = size;
    uint8_t *image = zalloc(c, (size_t)image_size);
    Segment *segments = zalloc(c, (nloads + 64) * sizeof(Segment));
    size_t nsegments = 0;
    for (int i = 0; i < main.nph; ++i) {
        const Phdr *p = &main.ph[i];
        if (!is_load(p)) continue;
        if (p->filesz > p->memsz) fail(c, "segment file size exceeds memory size");
        memcpy(image + p->vaddr, span(c, main.elf.data, main.elf.size, p->offset, p->filesz), (size_t)p->filesz);
        segments[nsegments++] = (Segment){p->vaddr, p->memsz, p->flags};
    }
    OutReloc *relocs = zalloc(c, (main.nrelocs + 1) * sizeof(OutReloc));
    size_t nrelocs = 0, relocs_capacity = main.nrelocs + 1;
    Map imports = {0};
    const char **names = zalloc(c, (main.nsymbols + 1) * sizeof(char *));
    size_t nnames = 0;
    for (size_t i = 0; i < main.nrelocs; ++i) {
        const Reloc *r = &main.relocs[i];
        if (!mapped(&main, r->target, 8)) fail(c, "relocation outside mapped segments: %#llx", (unsigned long long)r->target);
        if (r->kind == 8) {
            relocs[nrelocs++] = (OutReloc){r->target, 0, r->addend, 0};
        } else if (r->kind == 1 || r->kind == 6 || r->kind == 7) {
            if (r->symbol >= main.nsymbols) fail(c, "bad symbol index");
            const Symbol *s = &main.symbols[r->symbol];
            if (s->section) {
                if (s->section == 0xfff1) wr64(image + r->target, s->value + (uint64_t)r->addend);
                else relocs[nrelocs++] = (OutReloc){r->target, 0, (int64_t)(s->value + (uint64_t)r->addend), 0};
            } else {
                char key[16];
                snprintf(key, sizeof(key), "%u", r->symbol);
                int64_t *slot = map_slot(c, &imports, key, 0);
                if (!slot) {
                    slot = map_slot(c, &imports, key, 1);
                    *slot = (int64_t)nnames;
                    names[nnames++] = s->name;
                }
                uint64_t kind = s->type == 1 ? 2 : 1;
                if ((kind == 1 && r->addend) || !(0 <= r->addend && r->addend < 4096)) fail(c, "unsupported import addend");
                relocs[nrelocs++] = (OutReloc){r->target, kind, *slot, r->addend};
            }
        } else {
            fail(c, "unsupported relocation %u", r->kind);
        }
    }
    for (size_t i = 0; i < nnames; ++i)
        if (strlen(names[i]) >= 128) fail(c, "import name too long");
    Module libc;
    game_file(path, sizeof(path), game_dir, "sce_module/libc.prx");
    load_module(c, path, &libc);
    const uint64_t flags = (uint64_t)init_env_is_ret(c, &libc);

    /* link_modules.py: the bundled modules after the executable, one import table. */
    typedef struct { Identity identity; int64_t index; } Entry;
    Map identities = {0}; /* identity key (or "\x01" for none) -> position in entries */
    Entry *entries = zalloc(c, (nnames + 1) * sizeof(Entry));
    size_t nentries = 0, entries_capacity = nnames + 1;
    size_t names_capacity = nnames + 1;
    const char **rnames = zalloc(c, names_capacity * sizeof(char *));
    for (size_t i = 0; i < nnames; ++i) {
        Identity id = make_identity(c, &main, names[i]);
        const char *key = id.valid ? id.key : "\x01";
        int64_t *slot = map_slot(c, &identities, key, 0);
        if (slot) {
            entries[*slot].index = (int64_t)i; /* a repeated key keeps its place, takes the last index */
        } else {
            entries[nentries] = (Entry){id, (int64_t)i};
            *map_slot(c, &identities, key, 1) = (int64_t)nentries++;
        }
        rnames[i] = id.valid ? runtime_name(c, &id) : names[i];
    }
    size_t nrnames = nnames;
    Map exports = {0}, by_nid = {0};
    Export *export_list = NULL;
    size_t nexports = 0;
    int64_t *nid_first = NULL, *nid_count = NULL;
    size_t nid_slots = 0;
    uint64_t base = (size + 65535) & ~65535ull;
    patch_fs_loads(&main, image, image_size, 0);
    unsigned tls_module = 2;
    static const char *const module_names[] = {"libc.prx", "libSceFios2.prx"};
    uint64_t table[2][7];
    for (int mi = 0; mi < 2; ++mi) {
        Module mod;
        if (mi == 0) {
            mod = libc;
        } else {
            char rel[256];
            snprintf(rel, sizeof(rel), "sce_module/%s", module_names[mi]);
            game_file(path, sizeof(path), game_dir, rel);
            load_module(c, path, &mod);
        }
        uint64_t modsize = 0;
        for (int i = 0; i < mod.nph; ++i)
            if (is_load(&mod.ph[i]) && mod.ph[i].vaddr + mod.ph[i].memsz > modsize) modsize = mod.ph[i].vaddr + mod.ph[i].memsz;
        if (base + modsize > MAX_IMAGE) fail(c, "linked image exceeds probe limit");
        if (base + modsize > image_size) {
            image = grow(c, image, (size_t)(base + modsize));
            memset(image + image_size, 0, (size_t)(base + modsize - image_size));
            image_size = base + modsize;
        }
        for (int i = 0; i < mod.nph; ++i) {
            const Phdr *p = &mod.ph[i];
            if (!is_load(p)) continue;
            if (p->filesz > p->memsz) fail(c, "%s: invalid segment size", module_names[mi]);
            memcpy(image + base + p->vaddr, span(c, mod.elf.data, mod.elf.size, p->offset, p->filesz), (size_t)p->filesz);
            segments = grow(c, segments, (nsegments + 1) * sizeof(Segment));
            segments[nsegments++] = (Segment){base + p->vaddr, p->memsz, p->flags};
        }
        const Phdr *tls = NULL;
        for (int i = 0; i < mod.nph && !tls; ++i)
            if (mod.ph[i].type == 7) tls = &mod.ph[i];
        unsigned module_id = 0;
        if (tls) {
            if (tls->filesz > tls->memsz || !mapped(&mod, tls->vaddr, tls->memsz)) fail(c, "%s: unsupported TLS layout", module_names[mi]);
            module_id = tls_module++;
        }
        for (size_t i = 0; i < mod.nrelocs; ++i) {
            const Reloc *r = &mod.relocs[i];
            if (!mapped(&mod, r->target, 8)) fail(c, "%s: relocation outside image %#llx", module_names[mi], (unsigned long long)r->target);
            uint64_t target = r->target + base;
            if (nrelocs + 1 >= relocs_capacity) {
                relocs_capacity = relocs_capacity * 2 + 1024;
                relocs = grow(c, relocs, relocs_capacity * sizeof(OutReloc));
            }
            if (r->kind == 8) {
                relocs[nrelocs++] = (OutReloc){target, 0, (int64_t)(base + (uint64_t)r->addend), 0};
            } else if (r->kind == 16) {
                if (r->symbol >= mod.nsymbols) fail(c, "bad symbol index");
                const Symbol *s = &mod.symbols[r->symbol];
                if (*s->name || s->type != 3 || r->addend || !module_id) fail(c, "%s: unsupported external TLS relocation", module_names[mi]);
                wr64(image + target, module_id);
            } else if (r->kind == 1 || r->kind == 6 || r->kind == 7) {
                if (r->symbol >= mod.nsymbols) fail(c, "bad symbol index");
                const Symbol *s = &mod.symbols[r->symbol];
                if (s->section) {
                    if (s->section == 0xfff1) wr64(image + target, s->value + (uint64_t)r->addend);
                    else relocs[nrelocs++] = (OutReloc){target, 0, (int64_t)(base + s->value + (uint64_t)r->addend), 0};
                } else if (!s->identity.valid && s->binding == 2) {
                    wr64(image + target, 0); /* weak undefined (module_start/stop) */
                } else {
                    uint64_t kind = s->type == 1 ? 2 : 1;
                    if ((kind == 1 && r->addend) || !(0 <= r->addend && r->addend < 4096)) fail(c, "%s: unsupported import addend", module_names[mi]);
                    /* an import without an identity shares the executable's plain-named slot, as in
                     * link_modules.py (which has no name for a new one) */
                    int64_t *slot = map_slot(c, &identities, s->identity.valid ? s->identity.key : "\x01", 0);
                    int64_t index;
                    if (slot) {
                        index = entries[*slot].index;
                    } else {
                        if (!s->identity.valid) fail(c, "%s: import without an identity", module_names[mi]);
                        if (nentries == entries_capacity) {
                            entries_capacity *= 2;
                            entries = grow(c, entries, entries_capacity * sizeof(Entry));
                        }
                        if (nrnames == names_capacity) {
                            names_capacity *= 2;
                            rnames = grow(c, rnames, names_capacity * sizeof(char *));
                        }
                        index = (int64_t)nrnames;
                        entries[nentries] = (Entry){s->identity, index};
                        *map_slot(c, &identities, s->identity.key, 1) = (int64_t)nentries++;
                        rnames[nrnames++] = runtime_name(c, &s->identity);
                    }
                    relocs[nrelocs++] = (OutReloc){target, kind, index, r->addend};
                }
            } else {
                fail(c, "%s: unsupported relocation type %u", module_names[mi], r->kind);
            }
        }
        for (size_t i = 0; i < mod.nsymbols; ++i) {
            const Symbol *s = &mod.symbols[i];
            if (!(s->section && (s->binding == 1 || s->binding == 2) && s->identity.valid && (s->type == 1 || s->type == 2))) continue;
            if (!mapped(&mod, s->value, s->size ? s->size : 1)) fail(c, "%s: export outside load segments", module_names[mi]);
            if (map_slot(c, &exports, s->identity.key, 0)) fail(c, "%s: ambiguous export", module_names[mi]);
            export_list = grow(c, export_list, (nexports + 1) * sizeof(Export));
            export_list[nexports] = (Export){base + s->value, s->type == 1 ? 2 : 1, module_names[mi]};
            *map_slot(c, &exports, s->identity.key, 1) = (int64_t)nexports;
            char nidkey[160];
            snprintf(nidkey, sizeof(nidkey), "%s\x1f%s", s->identity.nid, s->identity.library.name);
            int64_t *ns = map_slot(c, &by_nid, nidkey, 0);
            if (!ns) {
                nid_first = grow(c, nid_first, (nid_slots + 1) * sizeof(int64_t));
                nid_count = grow(c, nid_count, (nid_slots + 1) * sizeof(int64_t));
                nid_first[nid_slots] = (int64_t)nexports;
                nid_count[nid_slots] = 0;
                ns = map_slot(c, &by_nid, nidkey, 1);
                *ns = (int64_t)nid_slots++;
            }
            ++nid_count[*ns];
            ++nexports;
        }
        patch_fs_loads(&mod, image, image_size, base);
        uint64_t init = 0;
        has_tag(&mod, 12, &init);
        table[mi][0] = base;
        table[mi][1] = modsize;
        table[mi][2] = base + init;
        table[mi][3] = tls ? base + tls->vaddr : 0;
        table[mi][4] = tls ? tls->memsz : 0;
        table[mi][5] = tls ? tls->filesz : 0;
        table[mi][6] = module_id;
        base = (base + modsize + 65535) & ~65535ull;
    }

    /* Bindings to the modules' exports, in the identities' order. */
    uint64_t (*bindings)[3] = zalloc(c, (nentries + 1) * sizeof(*bindings));
    size_t nbindings = 0;
    for (size_t i = 0; i < nentries; ++i) {
        const Entry *e = &entries[i];
        const Export *found = NULL;
        if (e->identity.valid) {
            int64_t *slot = map_slot(c, &exports, e->identity.key, 0);
            if (slot) found = &export_list[*slot];
            if (!found && !strcmp(e->identity.library.name, "libSceLibcInternal")) {
                char nidkey[160];
                snprintf(nidkey, sizeof(nidkey), "%s\x1f%s", e->identity.nid, "libc");
                int64_t *ns = map_slot(c, &by_nid, nidkey, 0);
                if (ns && nid_count[*ns] == 1) found = &export_list[nid_first[*ns]];
            }
        }
        if (found) {
            bindings[nbindings][0] = (uint64_t)e->index;
            bindings[nbindings][1] = found->address;
            bindings[nbindings++][2] = found->kind;
        }
    }

    const Phdr *main_tls = NULL;
    for (int i = 0; i < main.nph && !main_tls; ++i)
        if (main.ph[i].type == 7) main_tls = &main.ph[i];
    uint64_t tls_values[4] = {0, 0, 0, 0};
    if (main_tls) {
        tls_values[0] = main_tls->vaddr; tls_values[1] = main_tls->filesz;
        tls_values[2] = main_tls->memsz; tls_values[3] = main_tls->align;
        if (main_tls->filesz > main_tls->memsz || main_tls->memsz > 1024 * 1024 || main_tls->vaddr + main_tls->filesz > size)
            fail(c, "unsupported eboot TLS layout");
    }
    const Phdr *procparam = first_ph(c, &main, 0x61000001);

    Out o = {0};
    put(c, &o, "BBPROBE5", 8);
    put64(c, &o, image_size);
    put64(c, &o, main.entry);
    put64(c, &o, nsegments);
    put64(c, &o, nrelocs);
    put64(c, &o, nrnames);
    put64(c, &o, flags);
    put64(c, &o, procparam->vaddr);
    for (int i = 0; i < 4; ++i) put64(c, &o, tls_values[i]);
    put64(c, &o, 2);
    for (int mi = 0; mi < 2; ++mi)
        for (int k = 0; k < 7; ++k) put64(c, &o, table[mi][k]);
    put64(c, &o, nbindings);
    for (size_t i = 0; i < nbindings; ++i)
        for (int k = 0; k < 3; ++k) put64(c, &o, bindings[i][k]);
    for (size_t i = 0; i < nsegments; ++i) {
        put64(c, &o, segments[i].vaddr);
        put64(c, &o, segments[i].memsz);
        put64(c, &o, segments[i].flags);
    }
    for (size_t i = 0; i < nrnames; ++i) {
        uint8_t field[128] = {0};
        size_t n = strlen(rnames[i]);
        if (n >= 128) fail(c, "name too long");
        memcpy(field, rnames[i], n);
        put(c, &o, field, sizeof(field));
    }
    for (size_t i = 0; i < nrelocs; ++i) {
        put64(c, &o, relocs[i].target);
        put64(c, &o, relocs[i].kind);
        put64(c, &o, (uint64_t)relocs[i].value);
        put64(c, &o, (uint64_t)relocs[i].addend);
    }
    /* header + tables, then the image: written in one file without copying the image */
    char final_path[4096], part[4200];
    snprintf(final_path, sizeof(final_path), "%s/boot-linked.bin", out_dir);
    snprintf(part, sizeof(part), "%s.part", final_path);
    FILE *f = fopen(part, "wb");
    if (!f) fail(c, "cannot write %s", part);
    int ok = fwrite(o.data, 1, o.size, f) == o.size && fwrite(image, 1, (size_t)image_size, f) == (size_t)image_size;
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(part); fail(c, "cannot write %s", part); }
    remove(final_path);
    if (rename(part, final_path)) fail(c, "cannot rename %s", part);
}

/* ---- param.sfo (prepare.py sfo) ---- */
typedef struct { int found, number; uint32_t value; char text[512]; } SfoValue;

static SfoValue sfo_lookup(Ctx *c, const Buf *b, const char *wanted) {
    SfoValue result;
    memset(&result, 0, sizeof(result));
    const uint8_t *h = span(c, b->data, b->size, 0, 20);
    if (memcmp(h, "\0PSF", 4)) fail(c, "bad SFO signature");
    uint32_t keys = rd32(h + 8), values = rd32(h + 12), count = rd32(h + 16);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *e = span(c, b->data, b->size, 20 + i * 16ull, 16);
        uint16_t key = rd16(e), fmt = rd16(e + 2);
        uint32_t size = rd32(e + 4), off = rd32(e + 12);
        const uint8_t *k = span(c, b->data, b->size, (uint64_t)keys + key, 1);
        const uint8_t *nul = memchr(k, 0, (size_t)(b->size - (keys + key)));
        if (!nul) fail(c, "bad SFO key");
        const uint8_t *v = span(c, b->data, b->size, (uint64_t)values + off, size);
        if ((size_t)(nul - k) != strlen(wanted) || memcmp(k, wanted, (size_t)(nul - k))) continue;
        result.found = 1;
        if (fmt == 0x404) {
            if (size < 4) fail(c, "bad SFO number");
            result.number = 1;
            result.value = rd32(v);
            snprintf(result.text, sizeof(result.text), "%u", result.value);
        } else {
            size_t n = size;
            while (n && !v[n - 1]) --n;
            if (n >= sizeof(result.text)) n = sizeof(result.text) - 1;
            memcpy(result.text, v, n);
            result.text[n] = 0;
        }
    }
    return result;
}

/* content_profile.py: explicit offline AppContent metadata, SKU full. */
static void content_profile(Ctx *c, const char *game_dir, const char *out_dir) {
    char path[4096];
    game_file(path, sizeof(path), game_dir, "sce_sys/param.sfo");
    Buf b = read_file(c, path);
    Out o = {0};
    put(c, &o, "BBCONT01", 8);
    put32(c, &o, 3);
    for (int i = 1; i <= 4; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "USER_DEFINED_PARAM_%d", i);
        SfoValue v = sfo_lookup(c, &b, key);
        if (v.found && !v.number) fail(c, "invalid user-defined parameter");
        put32(c, &o, v.found ? v.value : 0);
    }
    write_file(c, out_dir, "content.bin", o.data, o.size);
}

static void release(Ctx *c) {
    for (size_t i = 0; i < c->nblocks; ++i) free(c->blocks[i]);
    free(c->blocks);
    c->blocks = NULL;
    c->nblocks = c->capacity = 0;
}

int game_prepare(const char *game_dir, const char *exe, const char *out_dir, char *error, size_t error_size) {
    Ctx c;
    memset(&c, 0, sizeof(c));
    char scratch[256];
    c.error = error && error_size ? error : scratch;
    c.error_size = error && error_size ? error_size : sizeof(scratch);
    c.error[0] = 0;
    if (setjmp(c.fail)) {
        release(&c);
        return -1;
    }
    prepare_and_link(&c, game_dir, exe, out_dir);
    release(&c);
    content_profile(&c, game_dir, out_dir);
    release(&c);
    return 0;
}

int game_sfo_value(const char *game_dir, const char *key, char *out, size_t out_size) {
    Ctx c;
    memset(&c, 0, sizeof(c));
    char error[256];
    c.error = error;
    c.error_size = sizeof(error);
    if (setjmp(c.fail)) {
        release(&c);
        return -1;
    }
    char path[4096];
    game_file(path, sizeof(path), game_dir, "sce_sys/param.sfo");
    Buf b = read_file(&c, path);
    SfoValue v = sfo_lookup(&c, &b, key);
    if (v.found && out && out_size) snprintf(out, out_size, "%s", v.text);
    release(&c);
    return v.found ? 0 : -1;
}
