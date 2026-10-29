/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include "runtime.h"
#include "guest_cpu.h"
#include "gpu/bbgpu.h"
#ifdef _WIN32
#include "red_zone.h"
#endif
#include "cpu_patch.h"
#if defined(__x86_64__)
#include <cpuid.h>
#elif !defined(__aarch64__)
#error This loader requires an x86-64 host (native guest code) or ARM64 (FEXCore).
#endif
#if !defined(__GNUC__)
#error This loader requires GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <errno.h>
#include <sys/prctl.h>
#endif
#ifdef BB_SDL_MAIN
/* g2-arm64: the APK's SDLActivity calls SDL_main (this file's main) in libmain.so. */
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

#if !defined(_WIN32) && defined(__aarch64__)
#define UC_PC(uc) ((uintptr_t)(uc)->uc_mcontext.pc)
#define UC_FP(uc) ((uintptr_t)(uc)->uc_mcontext.regs[29])
#define UC_ARG0(uc) ((uint64_t)(uc)->uc_mcontext.regs[0])
#elif !defined(_WIN32)
#define UC_PC(uc) ((uintptr_t)(uc)->uc_mcontext.gregs[REG_RIP])
#define UC_FP(uc) ((uintptr_t)(uc)->uc_mcontext.gregs[REG_RBP])
#define UC_ARG0(uc) ((uint64_t)(uc)->uc_mcontext.gregs[REG_RDI])
#endif
typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
int vulkan_smoke(void);

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
#ifdef _WIN32
/* BB_WATCH=offset:type,... (diagnosis): guest image variables printed every 5 seconds.
 * Types: f float, d double, b byte, i int32, q int64. */
/* BB_POKE=offset:type=value,... (experiments): written before each print. */
static void apply_pokes(void) {
    const char *p=getenv("BB_POKE");
    while (p && *p) {
        char *end; uint64_t offset=strtoull(p,&end,16);
        char type=*end==':' ? end[1] : 'f';
        const char *value=strchr(end,'=');
        if (!value) break;
        unsigned char *at=image+offset;
        if (type=='d') *(double *)at=strtod(value+1,NULL);
        else if (type=='b') *at=(unsigned char)strtoul(value+1,NULL,0);
        else if (type=='i') *(int32_t *)at=(int32_t)strtol(value+1,NULL,0);
        else *(float *)at=strtof(value+1,NULL);
        p=strchr(value,',');
        if (p) ++p;
    }
}
static DWORD WINAPI watch_thread(void *argument) {
    const char *list=argument;
    for (;;) {
        Sleep(5000);
        apply_pokes();
        char line[1024]; size_t used=0;
        for (const char *p=list; *p && used<sizeof(line)-64; ) {
            char *end; uint64_t offset=strtoull(p,&end,16);
            char type=*end==':' ? end[1] : 'f';
            const unsigned char *at=image+offset;
            if (type=='d') used+=snprintf(line+used,sizeof(line)-used," %llx=%g",(unsigned long long)offset,*(const double *)at);
            else if (type=='b') used+=snprintf(line+used,sizeof(line)-used," %llx=%u",(unsigned long long)offset,*at);
            else if (type=='i') used+=snprintf(line+used,sizeof(line)-used," %llx=%d",(unsigned long long)offset,*(const int32_t *)at);
            else if (type=='q') used+=snprintf(line+used,sizeof(line)-used," %llx=%lld",(unsigned long long)offset,*(const long long *)at);
            else used+=snprintf(line+used,sizeof(line)-used," %llx=%g",(unsigned long long)offset,*(const float *)at);
            p=strchr(end,',');
            if (!p) break;
            ++p;
        }
        printf("Watch:%s\n",line);
        fflush(stdout);
    }
    return 0;
}
#endif
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) return low;
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
#endif
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
#endif
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)image, argument);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)image;
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",caller-modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
#if !defined(_WIN32) && defined(__x86_64__)
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack. */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
/* The game is built for the PS4's AMD Jaguar CPU: AVX throughout, and BMI1 (andn, bextr, blsi, blsr,
 * tzcnt), MOVBE, LZCNT and POPCNT in thousands of places, its libc's string functions among them.
 * Without them it stopped at the first one with an illegal instruction (0xc000001d in libc.prx
 * while module 0 initialized), except LZCNT, which older Intel CPUs run as BSR (other results).
 * Exit code 24 with a message instead; BB_SKIP_CPU_CHECK=1 starts anyway. BMI1, LZCNT and MOVBE
 * are rewritten before the game starts (cpu_patch.c) unless BB_CPU_PATCH=0. */
static void check_cpu(void) {
#if !defined(__x86_64__)
    /* guest code runs on FEXCore */
#else
    if (getenv("BB_SKIP_CPU_CHECK")) return;
    unsigned a=0, b=0, c=0, d=0, basic=0, leaf7=0, extended=0;
    if (__get_cpuid(1,&a,&b,&c,&d)) basic=c;
    if (__get_cpuid_max(0,NULL)>=7) { __cpuid_count(7,0,a,b,c,d); leaf7=b; }    if (__get_cpuid(0x80000001,&a,&b,&c,&d)) extended=c;
    int avx=0;
    if (((basic>>27)&1) && ((basic>>28)&1)) { /* OSXSAVE and AVX: the OS also has to save YMM state */
        unsigned low, high;
        __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
        avx=(low&6)==6;
    }
    const struct { int present; const char *name; } features[]={
        {avx,"AVX"}, {(leaf7>>3)&1,"BMI1"}, {(basic>>22)&1,"MOVBE"}, {(extended>>5)&1,"LZCNT"},
        {(basic>>23)&1,"POPCNT"}, {(basic>>20)&1,"SSE4.2"},
    };
    char missing[64]="";
    const char *patch=getenv("BB_CPU_PATCH");
    const int rewrite=!patch || strcmp(patch,"0");
    int no_movbe=0;
    for (size_t i=0;i<sizeof(features)/sizeof(*features);++i) {
        if (features[i].present) continue;
        if (rewrite && (!strcmp(features[i].name,"BMI1") || !strcmp(features[i].name,"LZCNT") ||
                        !strcmp(features[i].name,"MOVBE"))) continue;
#ifdef _WIN32
        /* MOVBE alone (AMD FX): the fault handler does it (emulate_movbe). */
        if (!strcmp(features[i].name,"MOVBE")) { no_movbe=1; continue; }
#endif
        if (*missing) strcat(missing,", ");
        strcat(missing,features[i].name);
    }
    if (!*missing) {
        if (no_movbe) printf("CPU: no MOVBE, the game's MOVBE instructions are emulated (slower)\n");
        return;
    }
    unsigned words[12]={0};
    char brand[49]="";
    if (__get_cpuid_max(0x80000000,NULL)>=0x80000004)
        for (unsigned i=0;i<3;++i) __get_cpuid(0x80000002+i,&words[i*4],&words[i*4+1],&words[i*4+2],&words[i*4+3]);
    memcpy(brand,words,48);
    for (size_t n=strlen(brand); n && brand[n-1]==' '; --n) brand[n-1]=0;
    const char *name=brand;
    while (*name==' ') ++name;
    if (!*name) name="unknown processor";
    fprintf(stderr,"STOP: unsupported CPU %s: missing %s (needs Intel Core 4th generation or newer, or AMD Ryzen)\n",name,missing);
#ifdef _WIN32
    char text[512];
    snprintf(text,sizeof(text),"This processor cannot run the game:\n%s\n\nMissing instructions: %s\n\n"
             "The game uses instructions of the PS4's processor. They need an Intel Core of the 4th "
             "generation (Haswell, 2013) or newer, or an AMD Ryzen.",name,missing);
    MessageBoxA(NULL,text,"Uncharted",MB_OK|MB_ICONERROR|MB_SETFOREGROUND);
#endif
    exit(24);
#endif
}
#ifdef _WIN32
static void describe_address(char *out, size_t size, uintptr_t address) {
    HMODULE module=NULL;
    char name[MAX_PATH]="?";
    if (image && address-(uintptr_t)image<0x20000000) { snprintf(out,size,"guest offset 0x%" PRIxPTR,address-(uintptr_t)image); return; }
    uint64_t site;
    if (red_zone_site(address,&site)) { snprintf(out,size,"red-zone thunk for guest offset %#" PRIx64,site); return; }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)address,&module) && module) {
        GetModuleFileNameA(module,name,sizeof(name));
        const char *base=strrchr(name,'\\');
        snprintf(out,size,"%s+0x%" PRIxPTR,base ? base+1 : name,address-(uintptr_t)module);
        return;
    }
    snprintf(out,size,"%p",(void *)address);
}
static int fatal_reported;
static LONG fatal_exception(EXCEPTION_POINTERS *info) {
    if (__atomic_exchange_n(&fatal_reported,1,__ATOMIC_ACQ_REL)) { Sleep(INFINITE); }
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    const CONTEXT *c=info->ContextRecord;
    char where[MAX_PATH+64];
    describe_address(where,sizeof(where),(uintptr_t)c->Rip);
    if (record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2)
        fprintf(stderr,"Fault: %s access to %p at %s (thread %lu)\n",
                record->ExceptionInformation[0]==1 ? "write" : record->ExceptionInformation[0]==8 ? "execute" : "read",
                (void *)record->ExceptionInformation[1],where,GetCurrentThreadId());
    else
        fprintf(stderr,"Fault: exception 0x%08lx at %s (thread %lu)\n",record->ExceptionCode,where,GetCurrentThreadId());
    fprintf(stderr,"  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n",
            c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->Rbp,c->Rsp);
    if (gpu_enabled) bbgpu_dump_guest_writes(info);
    /* rbp chain (guest and host code keep frame pointers). */
    uintptr_t rbp=(uintptr_t)c->Rbp;
    for (int depth=0; depth<24 && rbp; ++depth) {
        uintptr_t frame[2];
        SIZE_T got=0;
        if (!ReadProcessMemory(GetCurrentProcess(),(void *)rbp,frame,sizeof(frame),&got) || got!=sizeof(frame) || frame[0]<=rbp) break;
        describe_address(where,sizeof(where),frame[1]);
        fprintf(stderr,"  #%d %s\n",depth,where);
        rbp=frame[0];
    }
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(),3);
    return EXCEPTION_CONTINUE_SEARCH;
}
/* AMD FX (Piledriver) has everything the PS4's code needs except MOVBE (byte-swapped loads and
 * stores, a few hundred places in each game). There it raises an illegal instruction: the load
 * or store is done here and the guest goes on after it (issue #251). Register operand forms do
 * not exist; segment and address-size prefixes are left to the fault report. */
static volatile LONG64 movbe_emulated;
/* Decodes a MOVBE with a memory operand at p (regs: Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi,
 * R8-R15, as in CONTEXT): its length, or 0 when it is not one. */
static int movbe_decode(const unsigned char *p, const uint64_t *regs, uint64_t rip, uint64_t *address,
                        int *reg, int *width, int *store) {
    int n=0, size16=0, rex=0;
    while (n<3 && p[n]==0x66) { size16=1; ++n; }
    if ((p[n]&0xf0)==0x40) rex=p[n++];
    if (p[n]!=0x0f || p[n+1]!=0x38 || (p[n+2]!=0xf0 && p[n+2]!=0xf1)) return 0;
    *store=p[n+2]==0xf1;
    n+=3;
    const unsigned char modrm=p[n++];
    const int mod=modrm>>6, rm=modrm&7;
    *reg=((modrm>>3)&7)|((rex&4)<<1);
    if (mod==3) return 0;
    uint64_t at=0;
    int rip_relative=0;
    if (rm==4) {
        const unsigned char sib=p[n++];
        const int index=((sib>>3)&7)|((rex&2)<<2), base=(sib&7)|((rex&1)<<3);
        if (index!=4) at+=regs[index]<<(sib>>6);
        if ((sib&7)==5 && mod==0) { int32_t d; memcpy(&d,p+n,4); n+=4; at+=(uint64_t)(int64_t)d; }
        else at+=regs[base];
    } else if (rm==5 && mod==0) {
        int32_t d; memcpy(&d,p+n,4); n+=4; at=(uint64_t)(int64_t)d; rip_relative=1;
    } else {
        at=regs[rm|((rex&1)<<3)];
    }
    if (mod==1) { at+=(uint64_t)(int64_t)(int8_t)p[n]; n+=1; }
    else if (mod==2) { int32_t d; memcpy(&d,p+n,4); n+=4; at+=(uint64_t)(int64_t)d; }
    if (rip_relative) at+=rip+(uint64_t)n;
    *address=at;
    *width=(rex&8) ? 8 : size16 ? 2 : 4;
    return n;
}
static int emulate_movbe(CONTEXT *c) {
    DWORD64 *regs=&c->Rax;
    uint64_t address;
    int reg, width, store;
    const int n=movbe_decode((const unsigned char *)(uintptr_t)c->Rip,(const uint64_t *)regs,c->Rip,
                             &address,&reg,&width,&store);
    if (!n) return 0;
    void *memory=(void *)(uintptr_t)address;
    if (store) {
        const uint64_t v=regs[reg];
        if (width==8) { uint64_t s=__builtin_bswap64(v); memcpy(memory,&s,8); }
        else if (width==4) { uint32_t s=__builtin_bswap32((uint32_t)v); memcpy(memory,&s,4); }
        else { uint16_t s=__builtin_bswap16((uint16_t)v); memcpy(memory,&s,2); }
    } else if (width==8) {
        uint64_t v; memcpy(&v,memory,8); regs[reg]=__builtin_bswap64(v);
    } else if (width==4) {
        uint32_t v; memcpy(&v,memory,4); regs[reg]=__builtin_bswap32(v);
    } else {
        uint16_t v; memcpy(&v,memory,2); regs[reg]=(regs[reg]&~(DWORD64)0xffff)|__builtin_bswap16(v);
    }
    c->Rip+=(DWORD64)n;
    InterlockedIncrement64(&movbe_emulated);
    return 1;
}
static LONG CALLBACK vectored_fault(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    DWORD code=record->ExceptionCode;
    /* Instructions this CPU lacks (cpu_patch.c): a rewritten breakpoint (Rip at the int3) or one
     * the scan missed continue in a thunk. */
    if (code==EXCEPTION_BREAKPOINT || code==EXCEPTION_ILLEGAL_INSTRUCTION) {
        uint64_t rip=info->ContextRecord->Rip;
        if (code==EXCEPTION_BREAKPOINT ? cpu_patch_breakpoint(&rip,rip) : cpu_patch_illegal(&rip)) {
            info->ContextRecord->Rip=rip;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2) {
        /* GPU page tracking (write-protected guest pages) is resolved first. */
        if (gpu_enabled && bbgpu_handle_fault(info,(void *)record->ExceptionInformation[1])) {
            red_zone_fault_handled(info->ContextRecord->Rip);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (runtime_fault_recover) {
            /* A speculative guest memory read (GPU draw preparation) faulted: the thread resumes
             * in bb_longjmp, outside the exception dispatcher, back at its recovery point. */
            unsigned long long *buffer=*runtime_fault_recover;
            runtime_fault_recover=NULL;
            CONTEXT *c=info->ContextRecord;
            c->Rsp=((c->Rsp-128)&~(DWORD64)15)-40;
            c->Rcx=(DWORD64)(uintptr_t)buffer;
            c->Rdx=1;
            c->Rip=(DWORD64)(uintptr_t)bb_longjmp;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code==EXCEPTION_ILLEGAL_INSTRUCTION && image &&
        (uintptr_t)info->ContextRecord->Rip-(uintptr_t)image<0x20000000 &&
        emulate_movbe(info->ContextRecord)) return EXCEPTION_CONTINUE_EXECUTION;
    if (code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_ILLEGAL_INSTRUCTION || code==EXCEPTION_PRIV_INSTRUCTION ||
        code==EXCEPTION_STACK_OVERFLOW || code==EXCEPTION_INT_DIVIDE_BY_ZERO || code==EXCEPTION_DATATYPE_MISALIGNMENT) {
        /* Faults in guest code, the loader or the runtime end the process with a report; drivers
         * and system libraries may handle their own exceptions. */
        uintptr_t rip=(uintptr_t)info->ContextRecord->Rip;
        HMODULE self=GetModuleHandleA(NULL), module=NULL;
        int ours=(image && rip-(uintptr_t)image<0x20000000) ||
                 (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)rip,&module) && module==self) ||
                 !module;
        if (ours) return fatal_exception(info);
    }
    /* BB_STRICT_HANDLES=1: a closed or invalid handle used anywhere is reported (diagnostics). */
    if (code==0xC0000008 /* STATUS_INVALID_HANDLE */) return fatal_exception(info);
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI unhandled_fault(EXCEPTION_POINTERS *info) { return fatal_exception(info); }
/* Guest code reads its TCB with `mov rax, gs:[0]` (link_*.py rewrote fs:[0]); on Windows
 * the displacement becomes the TEB TLS slot that holds the guest TCB (runtime_thread.c). */
uint32_t runtime_thread_tcb_offset(void);
static uint64_t patch_tcb_loads(const Segment *segments, uint64_t count) {
    static const unsigned char load[]={0x65,0x48,0x8b,0x04,0x25,0,0,0,0};
    uint32_t displacement=runtime_thread_tcb_offset();
    uint64_t patched=0;
    for (uint64_t s=0;s<count;++s) {
        if (!(segments[s].flags&1) || segments[s].size<sizeof(load)) continue;
        unsigned char *at=image+segments[s].address, *end=at+segments[s].size-sizeof(load);
        for (; at<=end; ++at)
            if (at[0]==0x65 && !memcmp(at,load,sizeof(load))) { memcpy(at+5,&displacement,4); ++patched; at+=sizeof(load)-1; }
    }
    return patched;
}
#endif
#ifndef _WIN32
static void fault(int sig, siginfo_t *info, void *context) {
    /* The guest CPU backend first (FEXCore: faults in translated code). */
    if (guest_cpu_signal(sig, info, context)) return;
#if defined(__x86_64__)
    /* Instructions this CPU lacks (cpu_patch.c): a rewritten breakpoint (rip after the int3) or
     * one the scan missed continue in a thunk. */
    if (sig == SIGTRAP || sig == SIGILL) {
        greg_t *rip = &((ucontext_t *)context)->uc_mcontext.gregs[REG_RIP];
        uint64_t to = (uint64_t)*rip;
        if (sig == SIGTRAP ? cpu_patch_breakpoint(&to, to - 1) : cpu_patch_illegal(&to)) { *rip = (greg_t)to; return; }
    }
#endif
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (gpu_enabled && sig == SIGSEGV && bbgpu_handle_fault(context, info->si_addr)) return;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    ucontext_t *uc = context;
    uintptr_t rip = UC_PC(uc);
    char line[512];
    Dl_info where;
    if (rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    /* g2-arm64: the guest state (FEX: the translated block's guest RIP) and the mappings around
     * the PC and the faulting address (JIT code has no module). */
    { char name[40]="  thread "; prctl(PR_GET_NAME,(unsigned long)(name+9),0,0,0); size_t l=strlen(name); name[l]=10;
      ssize_t w_=write(2,name,l+1); (void)w_; }
    { char guest[256]; int n=guest_cpu_describe_thread(guest,sizeof(guest));
      if (n>0) { ssize_t w_=write(2,guest,(size_t)n); (void)w_; }
      /* g2-arm64: every guest register, memory at rbx, the guest stack and the current fiber. */
      { char more[1536]; int m=guest_cpu_describe_registers(more,sizeof(more));
        if (m>0) { ssize_t w_=write(2,more,(size_t)m); (void)w_; }
        m=guest_cpu_describe_stack(more,sizeof(more));
        if (m>0) { ssize_t w_=write(2,more,(size_t)m); (void)w_; }
        m=runtime_fiber_describe(more,sizeof(more),guest_cpu_guest_rsp());
        if (m>0) { ssize_t w_=write(2,more,(size_t)m); (void)w_; } }
      /* The guest code bytes at that rip (an x86 dump identifies generated code). */
      unsigned long long grip=0; const char *r=strstr(guest,"rip=");
      if (n>0 && r && sscanf(r+4,"%llx",&grip)==1 && grip) {
          unsigned char code[24];
          struct iovec local={code,sizeof(code)}, remote={(void *)(uintptr_t)grip,sizeof(code)};
          if (process_vm_readv(getpid(),&local,1,&remote,1,0)==(ssize_t)sizeof(code)) {
              char hex[100]; int p=snprintf(hex,sizeof(hex),"  guest code:");
              for (int i=0;i<24 && p<(int)sizeof(hex)-4;++i) p+=snprintf(hex+p,sizeof(hex)-p," %02x",code[i]);
              hex[p++]='\n'; ssize_t w_=write(2,hex,(size_t)p); (void)w_;
          }
      } }
#if defined(__aarch64__)
    /* Host registers (FEX keeps the guest GPRs in fixed host registers while JIT code runs). */
    for (int r=0;r<31;r+=4) {
        char regs[160]; int p=0;
        for (int k=r;k<r+4 && k<31;++k)
            p+=snprintf(regs+p,sizeof(regs)-p," x%-2d=%016llx",k,(unsigned long long)uc->uc_mcontext.regs[k]);
        regs[p++]='\n'; ssize_t w_=write(2,regs,(size_t)p); (void)w_;
    }
#endif
    FILE *maps=fopen("/proc/self/maps","r");
    if (maps) {
        char m[512], prev[512]="";
        const uintptr_t addr=(uintptr_t)info->si_addr;
        /* g2-arm64: mappings (vm.max_map_count) by kind: guest memory (64-256 GiB), FEX's code
         * buffers, scudo, KGSL, ART, memfd, other named anonymous, unnamed, files. */
        static const char *const kind_names[]={"guest","FEX","scudo","kgsl","dalvik","memfd","[anon:...]","unnamed","files"};
        unsigned count=0, kinds[9]={0};
        while (fgets(m,sizeof(m),maps)) {
            unsigned long lo, hi;
            ++count;
            { const char *path=m; int field=0;
              while (*path && field<5) { while (*path && *path!=' ') ++path; while (*path==' ') ++path; ++field; }
              const unsigned long start=strtoul(m,NULL,16);
              int k;
              if (start>=0x1000000000UL && start<0x4000000000UL) k=0;
              else if (strstr(path,"FEXMem")) k=1;
              else if (strstr(path,"scudo")) k=2;
              else if (strstr(path,"kgsl")) k=3;
              else if (strstr(path,"dalvik")) k=4;
              else if (strstr(path,"memfd")) k=5;
              else if (*path=='[') k=6;
              else if (!*path || *path=='\n') k=7;
              else k=8;
              ++kinds[k]; }
            if (sscanf(m,"%lx-%lx",&lo,&hi)!=2) continue;
            if ((rip>=lo && rip<hi) || (addr>=lo && addr<hi)) { ssize_t w_=write(2,"  map ",6); w_=write(2,m,strlen(m)); (void)w_; }
            if (addr<lo && prev[0] && addr>=strtoul(prev,NULL,16)) {
                ssize_t w_=write(2,"  map below ",12); w_=write(2,prev,strlen(prev)); w_=write(2,"  map above ",12); w_=write(2,m,strlen(m)); (void)w_;
            }
            snprintf(prev,sizeof(prev),"%s",m);
        }
        fclose(maps);
        { char c[64]; int n=snprintf(c,sizeof(c),"  mappings: %u\n",count); ssize_t w_=write(2,c,(size_t)n); (void)w_; }
        { char c[256]; int n=snprintf(c,sizeof(c),"  mappings by kind:");
          for (int k=0;k<9;++k) n+=snprintf(c+n,sizeof(c)-n," %s %u",kind_names[k],kinds[k]);
          c[n++]='\n'; ssize_t w_=write(2,c,(size_t)n); (void)w_; }
    }
    if (gpu_enabled) bbgpu_dump_guest_writes(context);
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
    _exit(128 + sig);
}
#endif
#ifndef _WIN32
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
static uintptr_t exe_base;
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=UC_PC(uc), rbp=UC_FP(uc);
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,UC_ARG0(uc));
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    { char name[24]="  name="; prctl(PR_GET_NAME,(unsigned long)(name+7),0,0,0); size_t l=strlen(name); name[l]=10;
      ssize_t w_=write(2,name,l+1); (void)w_; }
    { char guest[256]; int n=guest_cpu_describe_thread(guest,sizeof(guest)); if (n>0) { ssize_t w_=write(2,guest,(size_t)n); (void)w_; }
      /* g2-arm64: a thread in a host function (a wait): its guest registers at the call, which
       * hold what it waits on (a counter and its target in a job system's wait loop). */
      if (n>0 && strstr(guest,"in host function")) { char regs[1536]; int m=guest_cpu_describe_registers(regs,sizeof(regs));
        if (m>0) { ssize_t w_=write(2,regs,(size_t)m); (void)w_; } } }
    { char stack[512]; int n=guest_cpu_describe_stack(stack,sizeof(stack)); if (n>0) { ssize_t w_=write(2,stack,(size_t)n); (void)w_; } }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
#if defined(__aarch64__)
        /* g2-arm64: return addresses carry a PAC signature in the top bits; XPACLRI (hint
         * space, a no-op without PAC) strips it from x30. */
        { register uintptr_t lr __asm__("x30") = rip; __asm__ volatile("hint #7" : "+r"(lr)); rip = lr; }
#endif
    }
}
/* g2-arm64: the executable mappings, to turn dumped addresses into module offsets
 * (Android: the renderer is libbbgpu.so, not the main binary). */
static void dump_exec_maps(void) {
    int fd=open("/proc/self/maps",O_RDONLY);
    if (fd<0) return;
    char buf[4096], line[512];
    size_t len=0;
    ssize_t n;
    while ((n=read(fd,buf,sizeof(buf)))>0)
        for (ssize_t i=0;i<n;++i) {
            if (len<sizeof(line)-1) line[len++]=buf[i];
            if (buf[i]!='\n') continue;
            line[len]=0;
            if (strstr(line," r-xp ")) { ssize_t w_=write(2,"  map ",6); w_=write(2,line,len); (void)w_; }
            len=0;
        }
    close(fd);
}
static void thread_dump(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; dump_frames(context); }
/* Every other thread dumps its own frames (SIGUSR2, thread_dump), one at a time. */
static void dump_other_threads(void) {
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self=gettid();
    while (dir>=0 && (n=syscall(SYS_getdents64,dir,buffer,sizeof(buffer)))>0)
        for (long at=0; at<n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d=(void *)(buffer+at);
            pid_t tid=(pid_t)strtol(d->name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=d->reclen;
        }
    if (dir>=0) close(dir);
    usleep(100000);
}
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    const char head[]="STOP: watchdog timeout; thread stacks:\n";
    { ssize_t written_=write(2,head,sizeof(head)-1); (void)written_; }
    dump_exec_maps();
    dump_frames(context);
    dump_other_threads();
    _exit(128 + sig);
}
/* g2-arm64: BB_DUMP_REQUEST=<file>: creating the file (adb shell touch) while the game runs
 * dumps every thread like the watchdog (host frames, guest state) and the game goes on; the
 * file is removed. For hangs on a device without root, where debuggerd cannot attach. */
static void *dump_on_request(void *arg) {
    const char *path=(const char *)arg;
    for (;;) {
        sleep(2);
        if (access(path,F_OK)) continue;
        unlink(path);
        const char head[]="DUMP: thread stacks on request:\n";
        { ssize_t written_=write(2,head,sizeof(head)-1); (void)written_; }
        dump_exec_maps();
        dump_other_threads();
        { const char fibers[]="DUMP: fibers:\n"; ssize_t w_=write(2,fibers,sizeof(fibers)-1); (void)w_; }
        runtime_fiber_dump();
        const char tail[]="DUMP: end\n";
        { ssize_t written_=write(2,tail,sizeof(tail)-1); (void)written_; }
    }
    return NULL;
}
/* g2-arm64: the stall report. No flip from the game for BB_STALL_REPORT_SECONDS (10; 0 turns it
 * off) while the app is in the foreground: every thread's stack goes into the log as with
 * BB_DUMP_REQUEST (the game goes on), at most twice a run. A hang on a player's device then
 * shows where the game waits in the log they copy, without adb. */
static void *stall_watch(void *arg) {
    const int limit=(int)(intptr_t)arg;
    int reports=0, stalled=0;
    while (reports<2) {
        sleep(2);
        char note[256]="";
        const int seconds=bbgpu_seconds_without_frame(note,sizeof(note));
        if (seconds<limit) { stalled=0; continue; }
        if (stalled) continue;
        stalled=1; ++reports;
        char head[128];
        const int n=snprintf(head,sizeof(head),"STALL: no frame from the game for %d s in the foreground; thread stacks:\nSTALL: ",seconds);
        { ssize_t written_=write(2,head,(size_t)n); written_=write(2,note,strlen(note)); (void)written_; }
        bbgpu_dump_stall_state();
        dump_exec_maps();
        dump_other_threads();
        { const char fibers[]="STALL: fibers:\n"; ssize_t w_=write(2,fibers,sizeof(fibers)-1); (void)w_; }
        runtime_fiber_dump();
        bbgpu_dump_recent_writes(3000); /* BB_WRITE_LOG=1: what the GPU side wrote last */
        guest_cpu_hle_trace_save(); /* BB_HLE_TRACE_RING: the calls that led here */
        const char tail[]="STALL: end\n";
        { ssize_t written_=write(2,tail,sizeof(tail)-1); (void)written_; }
    }
    return NULL;
}
/* g2-arm64: BB_PEEK_FILE=<file>: guest memory on request while the game runs (a hang on a
 * device without root or a debugger). <file> holds lines "<address> <length>" (hex, a length
 * up to 1 MiB); the reply is <file>.out, renamed into place when complete: per line a header
 * (address u64, length u32, status u32: 0 or an errno) and, when read, the bytes. The request
 * is removed; write it elsewhere and move it to <file> so that it is read whole. */
static void *peek_on_request(void *arg) {
    const char *path=(const char *)arg;
    char out[512], part[512];
    snprintf(out,sizeof(out),"%s.out",path);
    snprintf(part,sizeof(part),"%s.part",path);
    static unsigned char data[1<<20];
    for (;;) {
        usleep(200000);
        FILE *request=fopen(path,"r");
        if (!request) continue;
        FILE *reply=fopen(part,"wb");
        unsigned long long address, length;
        while (reply && fscanf(request,"%llx %llx",&address,&length)==2) {
            if (length>sizeof(data)) length=sizeof(data);
            /* process_vm_readv: an unmapped range is an error, not a fault. */
            struct iovec local={data,(size_t)length}, remote={(void *)(uintptr_t)address,(size_t)length};
            ssize_t got=process_vm_readv(getpid(),&local,1,&remote,1,0);
            uint32_t status=got==(ssize_t)length ? 0 : got<0 ? (uint32_t)errno : (uint32_t)EFAULT;
            uint64_t head_address=address;
            uint32_t head[2]={(uint32_t)length,status};
            fwrite(&head_address,8,1,reply); fwrite(head,4,2,reply);
            if (!status) fwrite(data,1,(size_t)length,reply);
        }
        fclose(request);
        unlink(path);
        if (reply) { fclose(reply); rename(part,out); }
    }
    return NULL;
}
#endif
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
/* BBPATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"BBPATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    const char *name=strrchr(path,'/'); /* g2-arm64: the file names the size (u1-960x540.bin) */
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased from %s\n",count,bytes,rebased,name ? name+1 : path);
}
/* BB_GUEST_HOOKS="offset:bytes,...,START-END": diagnostic call counters on guest functions.
 * The first `bytes` (>= 5, whole position-independent instructions) at each image offset move
 * to a thunk near the image, which counts the call and keeps its first two arguments; START-END
 * hooks every 16-byte aligned function in that range that begins with a frame-pointer prologue.
 * The counts that changed are printed every 2 s. */
#include <pthread.h>
typedef struct { volatile uint64_t calls, rdi, rsi, rdx, rcx, r8, r15; uint64_t offset, printed; } GuestHook;
static GuestHook *guest_hooks;
static size_t guest_hook_count;
static uint64_t guest_image_size;
/* A pointer argument into the image (or guest memory the tracker knows) shown as text. */
static void print_hook_string(const char *name, uint64_t value) {
    const unsigned char *p=(const unsigned char *)(uintptr_t)value;
    if (value<(uint64_t)(uintptr_t)image || value>=(uint64_t)(uintptr_t)image+guest_image_size-64) return;
    char text[65]; size_t n=0;
    while (n<64 && p[n]>=32 && p[n]<127) { text[n]=(char)p[n]; ++n; }
    text[n]=0;
    if (n>=3) printf("    %s -> \"%s\"\n",name,text);
}
static void *report_guest_hooks(void *unused) {
    (void)unused;
    for (;;) {
#ifdef _WIN32
        Sleep(2000);
#else
        sleep(2);
#endif
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC,&now);
        printf("Hooks at %.1f s\n",(double)now.tv_sec+now.tv_nsec/1e9);
        for (size_t i=0;i<guest_hook_count;++i) {
            GuestHook *h=&guest_hooks[i];
            uint64_t calls=h->calls;
            if (calls==h->printed) continue;
            printf("Hook %#" PRIx64 ": %" PRIu64 " calls (+%" PRIu64 ", last rdi %#" PRIx64 " rsi %#" PRIx64 " rdx %#" PRIx64
                   " rcx %#" PRIx64 " r8 %#" PRIx64 " r15 %#" PRIx64 ")\n",h->offset,calls,calls-h->printed,(uint64_t)h->rdi,
                   (uint64_t)h->rsi,(uint64_t)h->rdx,(uint64_t)h->rcx,(uint64_t)h->r8,(uint64_t)h->r15);
            h->printed=calls;
            print_hook_string("rdi",h->rdi); print_hook_string("rsi",h->rsi);
            print_hook_string("rdx",h->rdx); print_hook_string("rcx",h->rcx);
        }
        fflush(stdout);
    }
    return NULL;
}
/* BB_HOOK_DUMP=<hook offset>:<bytes>:<file>: every 20 ms the memory at the hook's last rdi
 * (an object) is appended to <file> as [double time][bytes] records (finding fields that follow
 * a value, e.g. a camera angle). */
static void *dump_hook_object(void *unused) {
    (void)unused;
    const char *spec=getenv("BB_HOOK_DUMP");
    char *end;
    const uint64_t offset=strtoull(spec,&end,0);
    const size_t bytes=*end==':' ? (size_t)strtoull(end+1,&end,0) : 0;
    FILE *f=*end==':' ? fopen(end+1,"wb") : NULL;
    GuestHook *h=NULL;
    for (size_t i=0;i<guest_hook_count;++i) if (guest_hooks[i].offset==offset) h=&guest_hooks[i];
    if (!f || !h || !bytes) { printf("BB_HOOK_DUMP: bad specification\n"); return NULL; }
    unsigned char *copy=malloc(bytes);
    const uint64_t before=getenv("BB_HOOK_DUMP_BEFORE") ? strtoull(getenv("BB_HOOK_DUMP_BEFORE"),NULL,0) : 0;
    for (;;) {
#ifdef _WIN32
        Sleep(50);
#else
        usleep(50000);
#endif
        const uint64_t object=h->rdi ? h->rdi-before : 0;
        if (!object) continue;
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((const void *)(uintptr_t)object,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
            (uint64_t)(uintptr_t)info.BaseAddress+info.RegionSize<object+bytes) continue;
#endif
        memcpy(copy,(const void *)(uintptr_t)object,bytes);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC,&now);
        const double t=(double)now.tv_sec+now.tv_nsec/1e9;
        fwrite(&t,sizeof(t),1,f);
        fwrite(copy,1,bytes,f);
        fflush(f);
    }
    return NULL;
}
/* Bytes of whole instructions (>= 5) a frame-pointer prologue at `f` can move, 0 if unknown. */
static size_t prologue_bytes(const unsigned char *f) {
    if (memcmp(f,"\x55\x48\x89\xe5",4)) return 0;
    if (f[4]==0x41 && f[5]>=0x50 && f[5]<=0x57) return 6;
    if (f[4]>=0x50 && f[4]<=0x57) return 5;
    if (f[4]==0x48 && f[5]==0x83 && f[6]==0xec) return 8;
    if (f[4]==0x48 && f[5]==0x81 && f[6]==0xec) return 11;
    return 0;
}
static void install_guest_hooks(Segment *segments, uint64_t ns) {
    const char *spec=getenv("BB_GUEST_HOOKS");
    if (!spec || !*spec) return;
    enum { MAX_HOOKS=16384, THUNK=80 };
    guest_hooks=calloc(MAX_HOOKS,sizeof(*guest_hooks));
    uint64_t end_of_image=0;
    for (uint64_t s=0;s<ns;++s) if (segments[s].address+segments[s].size>end_of_image) end_of_image=segments[s].address+segments[s].size;
    guest_image_size=end_of_image;
    unsigned char *thunks=NULL;
#ifdef _WIN32
    /* rel32 jumps: the thunks within 2 GiB of the image. */
    uint64_t base=((uint64_t)(uintptr_t)image+end_of_image+0xffff)&~(uint64_t)0xffff;
    for (uint64_t step=0;!thunks && step<(1900ull<<20);step+=1u<<16) {
        thunks=VirtualAlloc((void *)(uintptr_t)(base+step),(size_t)MAX_HOOKS*THUNK,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
        if (!thunks && (uint64_t)(uintptr_t)image>step+(2u<<20))
            thunks=VirtualAlloc((void *)(uintptr_t)(((uint64_t)(uintptr_t)image-step-(2u<<20))&~(uint64_t)0xffff),
                                (size_t)MAX_HOOKS*THUNK,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    }
#endif
    if (!thunks) fail("BB_GUEST_HOOKS: no thunk memory near the image");
    for (const char *p=spec;*p;) {
        char *end;
        uint64_t first=strtoull(p,&end,0), last=first, n=0;
        if (*end==':') n=strtoull(end+1,&end,0);
        else if (*end=='-') last=strtoull(end+1,&end,0);
        else fail("bad BB_GUEST_HOOKS entry");
        if (*end && *end!=',') fail("bad BB_GUEST_HOOKS entry");
        p=*end==',' ? end+1 : end;
        for (uint64_t offset=first;offset<=last;offset+=first==last ? 1 : 16) {
            if (!mapped(segments,ns,offset,16)) { if (first==last) fail("bad BB_GUEST_HOOKS entry"); continue; }
            unsigned char *f=image+offset;
            size_t moved=first==last ? (size_t)n : prologue_bytes(f);
            if (first!=last && !moved) continue;
            if (moved<5 || moved>16 || guest_hook_count==MAX_HOOKS) fail("bad BB_GUEST_HOOKS entry");
            GuestHook *h=&guest_hooks[guest_hook_count];
            unsigned char *t=thunks+guest_hook_count*THUNK;
            ++guest_hook_count;
            h->offset=offset;
            uintptr_t slot=(uintptr_t)h;
            size_t k=0;
            t[k++]=0x50;                                                /* push rax */
            t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&slot,8); k+=8;        /* movabs rax, hook */
            t[k++]=0xf0; t[k++]=0x48; t[k++]=0xff; t[k++]=0x00;         /* lock inc qword [rax] */
            t[k++]=0x48; t[k++]=0x89; t[k++]=0x78; t[k++]=0x08;         /* mov [rax+8], rdi */
            t[k++]=0x48; t[k++]=0x89; t[k++]=0x70; t[k++]=0x10;         /* mov [rax+16], rsi */
            t[k++]=0x48; t[k++]=0x89; t[k++]=0x50; t[k++]=0x18;         /* mov [rax+24], rdx */
            t[k++]=0x48; t[k++]=0x89; t[k++]=0x48; t[k++]=0x20;         /* mov [rax+32], rcx */
            t[k++]=0x4c; t[k++]=0x89; t[k++]=0x40; t[k++]=0x28;         /* mov [rax+40], r8 */
            t[k++]=0x4c; t[k++]=0x89; t[k++]=0x78; t[k++]=0x30;         /* mov [rax+48], r15 */
            t[k++]=0x58;                                                /* pop rax */
            memcpy(t+k,f,moved); k+=moved;                              /* the moved instructions */
            int32_t back=(int32_t)((int64_t)(uintptr_t)(f+moved)-(int64_t)(uintptr_t)(t+k+5));
            t[k++]=0xe9; memcpy(t+k,&back,4); k+=4;                     /* jmp offset+moved */
            int32_t to=(int32_t)((int64_t)(uintptr_t)t-(int64_t)(uintptr_t)(f+5));
            f[0]=0xe9; memcpy(f+1,&to,4); memset(f+5,0x90,moved-5);
        }
    }
    printf("Hooks: %zu guest functions counted\n",guest_hook_count);
    pthread_t thread;
    pthread_create(&thread,NULL,report_guest_hooks,NULL);
    if (getenv("BB_HOOK_DUMP")) pthread_create(&thread,NULL,dump_hook_object,NULL);
}
/* Game callbacks (runtime_guest_callbacks: mouse camera): a guest function whose code matches
 * the expected bytes calls a host function first, with rdi, rsi, the image base and r14 (Microsoft
 * x64 convention on every host). The thunk keeps the guest's registers and flags, so it also
 * works inside a function. */
static unsigned char *near_image_memory(size_t size) {
    unsigned char *memory=NULL;
#ifdef _WIN32
    /* rel32 jumps: within 2 GiB of the image. */
    uint64_t end_of_image=round_page(guest_image_size ? guest_image_size : 1);
    uint64_t base=((uint64_t)(uintptr_t)image+end_of_image+(32u<<20)+0xffff)&~(uint64_t)0xffff;
    for (uint64_t step=0;!memory && step<(1900ull<<20);step+=1u<<16) {
        memory=VirtualAlloc((void *)(uintptr_t)(base+step),size,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
        if (!memory && (uint64_t)(uintptr_t)image>step+(2u<<20))
            memory=VirtualAlloc((void *)(uintptr_t)(((uint64_t)(uintptr_t)image-step-(2u<<20))&~(uint64_t)0xffff),
                                size,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    }
#else
    (void)size;
#endif
    return memory;
}
/* Finds `pattern` (hex bytes, "??" = any byte) in the executable segments: returns the number of
 * matches and the first one (an image offset) in *found; the pattern length goes to *length. */
static size_t find_pattern(Segment *segments, uint64_t ns, const char *pattern, uint64_t *found, size_t *length) {
    unsigned char bytes[256], mask[256];
    size_t n=0, matches=0, first=0;
    for (const char *p=pattern;*p;) {
        if (*p==' ') { ++p; continue; }
        if (n==sizeof(bytes) || !p[1]) return 0;
        if (p[0]=='?' && p[1]=='?') { bytes[n]=0; mask[n++]=0; p+=2; continue; }
        char pair[3]={p[0],p[1],0}, *end;
        bytes[n]=(unsigned char)strtol(pair,&end,16);
        if (*end) return 0;
        mask[n++]=1; p+=2;
    }
    *length=n;
    while (first<n && !mask[first]) ++first;
    if (first==n) return 0;
    for (uint64_t s=0;s<ns;++s) {
        if (!(segments[s].flags&1) || segments[s].size<n) continue;
        const unsigned char *code=image+segments[s].address;
        for (uint64_t i=0;i+n<=segments[s].size;++i) {
            if (code[i+first]!=bytes[first]) continue;
            size_t k=0;
            while (k<n && (!mask[k] || code[i+k]==bytes[k])) ++k;
            if (k==n && !matches++) *found=segments[s].address+i;
        }
    }
    return matches;
}
static void install_game_callbacks(Segment *segments, uint64_t ns, uint64_t image_size) {
    size_t count=0;
    const RuntimeGuestCallback *callbacks=runtime_guest_callbacks(&count);
    unsigned char *thunks=NULL;
    size_t used=0;
    uint8_t installed[32]={0};
    uint64_t hook_offsets[32]; size_t hooks_done=0;
    guest_image_size=image_size;
    if (count>sizeof(installed)) count=sizeof(installed);
    /* A/B tests: BB_CALLBACKS=0 installs none, BB_CALLBACKS_SKIP=<text> none whose name contains the text */
    const char *off=getenv("BB_CALLBACKS"), *skip=getenv("BB_CALLBACKS_SKIP");
    if (off && off[0]=='0') { printf("Callbacks: disabled (BB_CALLBACKS=0)\n"); return; }
    for (size_t i=0;i<count;++i) {
        const RuntimeGuestCallback *c=&callbacks[i];
        if (skip && *skip && strstr(c->name,skip)) continue;
        uint64_t found=0;
        size_t length=0, matches=find_pattern(segments,ns,c->pattern,&found,&length);
        if (matches>1) { printf("Callbacks: %s: %zu matches, not installed\n",c->name,matches); continue; }
        if (!matches) continue;
        const uint64_t offset=found+c->skip;
        int hooked=0;
        for (size_t j=0;j<hooks_done;++j) if (hook_offsets[j]==offset) hooked=1;
        if (hooked) continue;                                          /* an earlier entry took this site */
        if (hooks_done<sizeof(hook_offsets)/sizeof(*hook_offsets)) hook_offsets[hooks_done++]=offset;
        if (!c->callback) {
            memcpy(image+offset,c->patch,c->moved);
            installed[i]=1;
            printf("Callbacks: %s at %#" PRIx64 " (patch)\n",c->name,offset);
            continue;
        }
        if (!thunks && !(thunks=near_image_memory(8192))) { printf("Callbacks: no thunk memory\n"); return; }
        if (used+320>8192) break;
        unsigned char *t=thunks+used, *f=image+offset;
        size_t k=0;
        /* pushfq; push rax rcx rdx r8-r11 rbx r12 r13 r15 (the RuntimeGuestRegs block); mov rbx, rsp;
         * and rsp, -16; sub rsp, 0xb0 (shadow space, the fifth argument, 8 xmm saves): works at a
         * function's start and inside it */
        static const unsigned char save[]={0x9c,0x50,0x51,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,0x53,
                                           0x41,0x54,0x41,0x55,0x41,0x57,
                                           0x48,0x89,0xe3,0x48,0x83,0xe4,0xf0,0x48,0x81,0xec,0xb0,0x00,0x00,0x00};
        memcpy(t+k,save,sizeof(save)); k+=sizeof(save);
        for (int x=0;x<8;++x) {                                     /* movdqu [rsp+0x30+16x], xmmx */
            const uint32_t disp=0x30u+16u*(uint32_t)x;
            t[k++]=0xf3; t[k++]=0x0f; t[k++]=0x7f; t[k++]=(unsigned char)(0x84|x<<3); t[k++]=0x24;
            memcpy(t+k,&disp,4); k+=4;
        }
        t[k++]=0x48; t[k++]=0x89; t[k++]=0xf9;                      /* mov rcx, rdi */
        t[k++]=0x48; t[k++]=0x89; t[k++]=0xf2;                      /* mov rdx, rsi */
        const uintptr_t base=(uintptr_t)image, host=(uintptr_t)c->callback;
        t[k++]=0x49; t[k++]=0xb8; memcpy(t+k,&base,8); k+=8;        /* movabs r8, image */
        t[k++]=0x4d; t[k++]=0x89; t[k++]=0xf1;                      /* mov r9, r14 */
        t[k++]=0x48; t[k++]=0x89; t[k++]=0x5c; t[k++]=0x24; t[k++]=0x20; /* mov [rsp+0x20], rbx (regs) */
        t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&host,8); k+=8;        /* movabs rax, callback */
        t[k++]=0xff; t[k++]=0xd0;                                   /* call rax */
        for (int x=0;x<8;++x) {                                     /* movdqu xmmx, [rsp+0x30+16x] */
            const uint32_t disp=0x30u+16u*(uint32_t)x;
            t[k++]=0xf3; t[k++]=0x0f; t[k++]=0x6f; t[k++]=(unsigned char)(0x84|x<<3); t[k++]=0x24;
            memcpy(t+k,&disp,4); k+=4;
        }
        /* mov rsp, rbx; pop r15 r13 r12 rbx r11-r8 rdx rcx rax; popfq */
        static const unsigned char restore[]={0x48,0x89,0xdc,0x41,0x5f,0x41,0x5d,0x41,0x5c,0x5b,
                                              0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5a,0x59,0x58,0x9d};
        memcpy(t+k,restore,sizeof(restore)); k+=sizeof(restore);
        memcpy(t+k,f,c->moved); k+=c->moved;                       /* the moved instructions */
        int32_t back=(int32_t)((int64_t)(uintptr_t)(f+c->moved)-(int64_t)(uintptr_t)(t+k+5));
        t[k++]=0xe9; memcpy(t+k,&back,4); k+=4;
        int32_t to=(int32_t)((int64_t)(uintptr_t)t-(int64_t)(uintptr_t)(f+5));
        f[0]=0xe9; memcpy(f+1,&to,4); memset(f+5,0x90,c->moved-5);
        used+=(k+15)&~(size_t)15;
        installed[i]=1;
        printf("Callbacks: %s at %#" PRIx64 "\n",c->name,offset);
    }
    /* Names start with the game ("U2 ..."): a callback missing from a game whose other callbacks
     * attached is reported; callbacks of the other games are not. */
    int any=0;
    for (size_t i=0;i<count;++i) {
        int seen=0, ok=0, group=0;
        const size_t game=strcspn(callbacks[i].name," ");
        for (size_t j=0;j<count;++j) {
            if (!strcmp(callbacks[j].name,callbacks[i].name)) { if (j<i) seen=1; if (installed[j]) ok=1; }
            if (installed[j] && !strncmp(callbacks[j].name,callbacks[i].name,game+1)) group=1;
        }
        any|=ok;
        if (!seen && !ok && group) printf("Callbacks: %s not found\n",callbacks[i].name);
    }
    if (!any) printf("Callbacks: none of the known camera and pad code was found (unknown build?)\n");
}
/* Naughty Dog's FrameBegin (ndlib/render/fg-draw-mgr.cpp) asserts that the frame it recycles,
 * three back, has its GPU end-of-pipe tick (set by the game's EOP interrupt handler). It never
 * waits: on PS4 the GPU always keeps up. Here the GPU side can fall behind (shaders building on a
 * first run, an upscaler starting) and the game stopped right after U1's first cutscene. The
 * assertion's branch is found through its message, in any version of the three games, and the
 * failed check now waits (pause, check again) instead. */
static void patch_eop_tick_wait(uint64_t image_size) {
    static const char message[]="GetRenderFrameParams(frameToFreeInstancesFor)->m_gfxEopTick";
    unsigned char *text=NULL;
    for (uint64_t i=0;i+sizeof(message)<=image_size;++i)
        if (image[i]=='G' && !memcmp(image+i,message,sizeof(message))) { text=image+i; break; }
    if (!text) return;
    int patched=0;
    for (uint64_t at=0;at+7<=image_size;++at) {
        /* lea reg, [rip+rel32] -> the message (REX.W, any register) */
        if ((image[at]&0xfb)!=0x48 || image[at+1]!=0x8d || (image[at+2]&0xc7)!=0x05) continue;
        int32_t rel; memcpy(&rel,image+at+3,4);
        if (image+at+7+rel!=text) continue;
        /* The assertion path ends in the int 0x41 shortly after the message is loaded. */
        unsigned char *trap=NULL;
        for (unsigned char *q=image+at+7;q<image+at+0x80 && q+2<=image+image_size;++q)
            if (q[0]==0xcd && q[1]==0x41) { trap=q; break; }
        if (!trap) continue;
        /* Up to 0x100 bytes before the trap: cmp qword [base(+index*scale)+disp8/32], 0 (REX.W 83 /7,
         * mod 01/10, with or without SIB: 1.00 has [rax+0xa08], US 1.02 [rax+rcx+0xa08]), then a short or
         * near jne past the trap (the assertion is the fall-through) or je to the assertion. The assertion
         * path is dead code ending in the trap; its first 7 bytes become pause; jmp back to the cmp. */
        for (unsigned char *c=trap-2;c>image && trap-c<=0x100;--c) {
            if ((c[0]&0xf8)!=0x48 || c[1]!=0x83 || ((c[2]>>3)&7)!=7) continue;
            const int mod=c[2]>>6, sib=(c[2]&7)==4;
            if (mod!=1 && mod!=2) continue;
            const int length=3+sib+(mod==2 ? 4 : 1);
            if (c[length]!=0x00) continue;
            unsigned char *j=c+length+1, *target, *after;
            int jne;
            if (j[0]==0x75 || j[0]==0x74) { target=j+2+(int8_t)j[1]; after=j+2; jne=j[0]==0x75; }
            else if (j[0]==0x0f && (j[1]==0x85 || j[1]==0x84)) {
                int32_t d; memcpy(&d,j+2,4); target=j+6+d; after=j+6; jne=j[1]==0x85;
            } else continue;
            unsigned char *path=jne ? after : target;
            if (jne ? target<=trap : target<=c) continue;
            if (path<c || path+7>trap+2) continue;
            const int32_t back=(int32_t)(c-(path+7));
            path[0]=0xf3; path[1]=0x90; path[2]=0xe9; memcpy(path+3,&back,4);
            printf("Frame pacing: the EOP tick check at %#" PRIx64 " waits for the GPU\n",(uint64_t)(c-image));
            ++patched;
            break;
        }
    }
    if (!patched) printf("Frame pacing: EOP tick check not found\n");
}
/* g2-arm64: GetRenderFrameParams (ndlib/frame-params.cpp) halts when asked for a frame newer than
 * the last one the game registered ("frame number out of range. last: 1, asked: 2"). The flip
 * handler asks for the frame it was just told about; on PS4 the game always registered it first.
 * When the game thread is slow (guest code under a JIT compiling cold code) the flip event wins
 * and the game stopped at its first frame. The range check (cmp/sub r64, r64; jl rel8/rel32 into
 * the halt block, up to 0x60 bytes before the message's lea, often in a cold section far away) now
 * jumps back to the load of `last` (mov r64, [rip+disp32], up to 40 bytes before the compare) and
 * waits for it instead. U1 1.02: 15 inlined copies, every load reads the same global. */
static void patch_frame_params_wait(uint64_t image_size) {
    static const char message[]="GetRenderFrameParams called with a frame number out of range";
    unsigned char *text=NULL;
    for (uint64_t i=0;i+sizeof(message)<=image_size;++i)
        if (image[i]=='G' && !memcmp(image+i,message,sizeof(message)-1)) { text=image+i; break; }
    if (!text) return;
    uint64_t *refs=NULL; size_t nrefs=0, cap=0;
    for (uint64_t at=128;at+7<=image_size;++at) {
        /* lea r64, [rip+rel32] -> the message */
        if ((image[at]&0xfb)!=0x48 || image[at+1]!=0x8d || (image[at+2]&0xc7)!=0x05) continue;
        int32_t rel; memcpy(&rel,image+at+3,4);
        if (image+at+7+rel!=text) continue;
        if (nrefs==cap && !(refs=realloc(refs,(cap=cap?cap*2:32)*sizeof(*refs)))) return;
        refs[nrefs++]=at;
    }
    int patched=0;
    for (uint64_t p=48;p+6<=image_size;++p) {
        /* jl rel8 / jl rel32 right after cmp|sub r64, r64 (REX.W 39/29/3b/2b, register form) */
        size_t length;
        int64_t target;
        if (image[p]==0x7c) { length=2; target=(int64_t)p+2+(int8_t)image[p+1]; }
        else if (image[p]==0x0f && image[p+1]==0x8c) { int32_t r; memcpy(&r,image+p+2,4); length=6; target=(int64_t)p+6+r; }
        else continue;
        const unsigned char *c=image+p-3;
        if ((c[0]&0xf8)!=0x48 || (c[1]!=0x29 && c[1]!=0x39 && c[1]!=0x2b && c[1]!=0x3b) || (c[2]>>6)!=3) continue;
        int into_halt=0;
        for (size_t r=0;r<nrefs && !into_halt;++r) into_halt=target<=(int64_t)refs[r] && target>=(int64_t)refs[r]-0x60;
        if (!into_halt) continue;
        unsigned char *load=NULL;
        for (unsigned char *m=image+p-3-7; m>=image+p-3-40; --m)
            if ((m[0]&0xfb)==0x48 && m[1]==0x8b && (m[2]&0xc7)==0x05) { load=m; break; }
        if (!load) continue;
        const int64_t back=(int64_t)(load-(image+p+length));
        if (length==2) {
            if (back<-128) continue;
            image[p+1]=(unsigned char)(int8_t)back;
        } else {
            const int32_t r=(int32_t)back;
            memcpy(image+p+2,&r,4);
        }
        ++patched;
    }
    free(refs);
    printf("Frame pacing: %d GetRenderFrameParams range checks (%zu messages) wait for the game\n",patched,nrefs);
}
/* The single offset in the image where a byte pattern ("4d8d7001 74??", ?? = any byte) occurs,
 * else -1 (none or several). */
static int64_t find_unique_pattern(uint64_t image_size, const char *pattern) {
    unsigned char bytes[64], mask[64];
    size_t n=0;
    for (const char *p=pattern;*p && n<sizeof(bytes);) {
        if (*p==' ') { ++p; continue; }
        mask[n]=p[0]!='?';
        bytes[n]=mask[n] ? (unsigned char)strtoul((char[]){p[0],p[1],0},NULL,16) : 0;
        ++n;
        p+=2;
    }
    int64_t found=-1;
    for (uint64_t at=0;at+n<=image_size;++at) {
        if (image[at]!=bytes[0]) continue;
        size_t i=1;
        while (i<n && (!mask[i] || image[at+i]==bytes[i])) ++i;
        if (i<n) continue;
        if (found>=0) return -1;
        found=(int64_t)at;
    }
    return found;
}
/* g2-arm64 diagnosis (issue #17): BB_COUNTER_LOG=<file> (U2): a ring of the job manager's counter
 * events - a job or barrier done whose decrement left the counter at or below 0 (0xf3e09c, with
 * three slots of the worker's frame, rbp-0x88/-0x80/-0x70: an object and a sequence number seen
 * there, not the job's function), a SetCounter/decrement notify with a value at or below 0
 * (0xf29440, with its caller) and every counter freed (0xf29230, with its value and caller).
 * The first value between -15 and -1 (a counter decremented once too often; frame sequence
 * counters are far below) freezes the ring, and a thread writes it to <file>
 * (scripts/g2/counter_log.py). The stubs live in the zero tail of the code segment (0x13e3000). */
typedef struct { uint64_t counter, value, a, b, c, tsc, rsp, kind; } CounterEvent;
enum { COUNTER_EVENTS = 1 << 16 };
static struct { volatile uint64_t next, frozen, pad[6]; CounterEvent events[COUNTER_EVENTS]; } counter_log;
/* BB_COUNTER_LOG_ARM=<s>: the stubs log nothing until s seconds after the start (U2's level load
 * takes one counter to -1 every time, deterministically: not what is looked for);
 * BB_COUNTER_LOG_CAPTURES=<n>: after a ring is written the log starts again, n rings at most
 * (<file>, <file>.2, ...), so a run counts how often it happens. */
static long counter_log_arm, counter_log_captures = 1;
static void *counter_log_writer(void *arg) {
    const char *path = arg;
    if (counter_log_arm > 0) {
        sleep((unsigned)counter_log_arm);
        counter_log.next = 0;
        __atomic_store_n(&counter_log.frozen, 0, __ATOMIC_RELEASE);
        printf("Counter log: logging from now on (%ld s after the start)\n", counter_log_arm);
        fflush(stdout);
    }
    for (long n = 1; n <= counter_log_captures; ++n) {
        while (__atomic_load_n(&counter_log.frozen, __ATOMIC_ACQUIRE) != 1) usleep(200000);
        usleep(100000); /* events in flight */
        char name[512];
        if (n == 1) snprintf(name, sizeof(name), "%s", path);
        else snprintf(name, sizeof(name), "%s.%ld", path, n);
        FILE *f = fopen(name, "wb");
        const int ok = f && fwrite(&counter_log, sizeof(counter_log), 1, f) == 1;
        if (f) fclose(f);
        printf("Counter log: a counter went below 0 (event %" PRIu64 "), ring %s %s\n",
               (uint64_t)counter_log.next - 1, ok ? "written to" : "NOT written to", name);
        fflush(stdout);
        if (n < counter_log_captures) {
            counter_log.next = 0; /* the stubs skip while frozen */
            __atomic_store_n(&counter_log.frozen, 0, __ATOMIC_RELEASE);
        }
    }
    return NULL;
}
static int route_wait_and_free(uint64_t image_size);
static void install_counter_log(uint64_t image_size) {
    const char *path = getenv("BB_COUNTER_LOG");
    if (!path || !*path) return;
    const uint64_t base = 0x400000, cave = 0x13e3000 - base;
    static const struct { uint64_t at; unsigned char bytes[6]; size_t n; } expect[] = {
        {0xf3e09c, {0xe8, 0x4f, 0xdb, 0xff, 0xff}, 5},       /* call 0xf3bbf0 after the decrement */
        {0xf29440, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, 6}, /* the notify's prologue */
        {0xf29230, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57}, 6}, /* FreeCounter's prologue */
    };
    if (cave + 1024 > image_size) { printf("Counter log: no code cave\n"); return; }
    for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); ++i)
        if (memcmp(image + expect[i].at - base, expect[i].bytes, expect[i].n)) {
            printf("Counter log: %#" PRIx64 " is not U2's code, off\n", expect[i].at);
            return;
        }
    for (size_t i = 0; i < 1024; ++i)
        if (image[cave + i]) { printf("Counter log: the code cave is not empty, off\n"); return; }
    route_wait_and_free(image_size); /* WaitAndFree frees through FreeCounter: logged as well */
    unsigned char *t = image + cave;
    size_t k = 0;
    const uint64_t log_base = (uint64_t)(uintptr_t)&counter_log;
#define B(...) do { const unsigned char b_[] = {__VA_ARGS__}; memcpy(t + k, b_, sizeof(b_)); k += sizeof(b_); } while (0)
#define REL32(target) do { const int32_t r_ = (int32_t)((int64_t)(target) - (int64_t)(base + cave + k + 4)); memcpy(t + k, &r_, 4); k += 4; } while (0)
    /* rax: the ring; rcx: the event; a value above 0 or a frozen ring skips (rel8 to `skip`). */
#define APPEND(skip_positive)                                                                      \
    do {                                                                                           \
        if (skip_positive) { B(0x48, 0x85, 0xf6); B(0x7f); jg_at = k; B(0x00); } /* test rsi; jg */ \
        B(0x48, 0xb8); memcpy(t + k, &log_base, 8); k += 8;      /* movabs rax, ring */           \
        B(0x48, 0x83, 0x78, 0x08, 0x00); B(0x75); jne_at = k; B(0x00); /* frozen? */             \
        B(0xb9, 0x01, 0x00, 0x00, 0x00);                        /* mov ecx, 1 */                  \
        B(0xf0, 0x48, 0x0f, 0xc1, 0x08);                        /* lock xadd [rax], rcx */        \
        B(0x81, 0xe1, 0xff, 0xff, 0x00, 0x00);                  /* and ecx, 0xffff */             \
        B(0x48, 0xc1, 0xe1, 0x06);                              /* shl rcx, 6 */                  \
        B(0x48, 0x8d, 0x4c, 0x08, 0x40);                        /* lea rcx, [rax+rcx+0x40] */     \
        B(0x48, 0x89, 0x39);                                    /* mov [rcx], rdi */              \
    } while (0)
#define FINISH(kind, freeze)                                                                       \
    do {                                                                                           \
        B(0x49, 0x89, 0xc0);                                    /* mov r8, rax */                 \
        B(0x0f, 0x31, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xc2); /* rdtsc; shl rdx,32; or rdx,rax */ \
        B(0x48, 0x89, 0x51, 0x28);                              /* mov [rcx+40], rdx */           \
        B(0x48, 0x89, 0x61, 0x30);                              /* mov [rcx+48], rsp */           \
        B(0x48, 0xc7, 0x41, 0x38, kind, 0x00, 0x00, 0x00);      /* mov qword [rcx+56], kind */    \
        if (freeze) {                                                                              \
            B(0x48, 0x83, 0xfe, 0xf0); B(0x7e); jle_at = k; B(0x00); /* cmp rsi,-16; jle skip */ \
            B(0x48, 0x85, 0xf6); B(0x79); jns_at = k; B(0x00);  /* test rsi; jns skip */          \
            B(0x49, 0xc7, 0x40, 0x08, 0x01, 0x00, 0x00, 0x00);  /* mov qword [r8+8], 1 */         \
        }                                                                                          \
    } while (0)
#define LAND(at) do { if (at) t[at] = (unsigned char)(k - (at) - 1); } while (0)
    size_t jg_at = 0, jne_at = 0, jle_at = 0, jns_at = 0;
    /* A: a job or barrier done (the worker's frame: entry at rbp-0x88). */
    const uint64_t stub_a = base + cave + k;
    APPEND(1);
    B(0x48, 0x89, 0x71, 0x08);                                  /* mov [rcx+8], rsi */
    B(0x48, 0x8b, 0x95, 0x78, 0xff, 0xff, 0xff, 0x48, 0x89, 0x51, 0x10); /* fn */
    B(0x48, 0x8b, 0x55, 0x80, 0x48, 0x89, 0x51, 0x18);          /* data */
    B(0x48, 0x8b, 0x55, 0x90, 0x48, 0x89, 0x51, 0x20);          /* wait counter */
    FINISH(1, 1);
    LAND(jg_at); LAND(jne_at); LAND(jle_at); LAND(jns_at);
    B(0xe9); REL32(0xf3bbf0);                                   /* jmp 0xf3bbf0 */
    k = (k + 15) & ~(size_t)15;
    /* B: the notify (rdi counter, rsi value, [rsp] its caller). */
    jg_at = jne_at = jle_at = jns_at = 0;
    const uint64_t stub_b = base + cave + k;
    APPEND(1);
    B(0x48, 0x89, 0x71, 0x08);                                  /* mov [rcx+8], rsi */
    B(0x48, 0x8b, 0x14, 0x24, 0x48, 0x89, 0x51, 0x10);          /* caller */
    B(0x48, 0xc7, 0x41, 0x18, 0, 0, 0, 0, 0x48, 0xc7, 0x41, 0x20, 0, 0, 0, 0);
    FINISH(2, 1);
    LAND(jg_at); LAND(jne_at); LAND(jle_at); LAND(jns_at);
    B(0x55, 0x48, 0x89, 0xe5, 0x41, 0x57);                      /* the moved prologue */
    B(0xe9); REL32(0xf29446);
    k = (k + 15) & ~(size_t)15;
    /* C: FreeCounter (rdi counter, [rsp] its caller): every free, with the value left. */
    jg_at = jne_at = jle_at = jns_at = 0;
    const uint64_t stub_c = base + cave + k;
    APPEND(0);
    B(0x48, 0x8b, 0x57, 0x20, 0x48, 0x89, 0x51, 0x08);          /* value */
    B(0x48, 0x8b, 0x14, 0x24, 0x48, 0x89, 0x51, 0x10);          /* caller */
    B(0x48, 0xc7, 0x41, 0x18, 0, 0, 0, 0, 0x48, 0xc7, 0x41, 0x20, 0, 0, 0, 0);
    FINISH(4, 0);
    LAND(jne_at);
    B(0x55, 0x48, 0x89, 0xe5, 0x41, 0x57);
    B(0xe9); REL32(0xf29236);
#undef LAND
#undef FINISH
#undef APPEND
#undef REL32
#undef B
    /* The hooks: a call into stub A, jumps into B and C. */
    unsigned char *p = image + 0xf3e09c - base;
    int32_t r = (int32_t)((int64_t)stub_a - (int64_t)(0xf3e09c + 5));
    p[0] = 0xe8; memcpy(p + 1, &r, 4);
    p = image + 0xf29440 - base;
    r = (int32_t)((int64_t)stub_b - (int64_t)(0xf29440 + 5));
    p[0] = 0xe9; memcpy(p + 1, &r, 4); p[5] = 0x90;
    p = image + 0xf29230 - base;
    r = (int32_t)((int64_t)stub_c - (int64_t)(0xf29230 + 5));
    p[0] = 0xe9; memcpy(p + 1, &r, 4); p[5] = 0x90;
    const char *arm = getenv("BB_COUNTER_LOG_ARM"), *captures = getenv("BB_COUNTER_LOG_CAPTURES");
    counter_log_arm = arm ? strtol(arm, NULL, 10) : 0;
    const long n = captures ? strtol(captures, NULL, 10) : 1;
    if (n > 1) counter_log_captures = n;
    if (counter_log_arm > 0) counter_log.frozen = 2; /* the stubs skip until the writer arms them */
    pthread_t thread;
    if (pthread_create(&thread, NULL, counter_log_writer, (void *)path) == 0) pthread_detach(thread);
    printf("Counter log: armed (%zu bytes of stubs at %#" PRIx64 ", ring at %p), written to %s\n", k,
           base + cave, (void *)&counter_log, path);
}
/* g2-arm64 (issue #17): U2's job counters freed while a job still holds them. A job decrements the
 * counter object it took with its entry, whoever owns it by then: neither free path checks for
 * pending entries, and the pool (2048 objects, bitmap at mgr+0xb00) hands out the lowest free one
 * first, so such a job's decrement lands on the next owner: a round reaching its target a job early
 * (0.2.7's movie stop; 0.2.8's crash in the particles' DrawReg job, its list released before
 * `UpdateRender` ran), and the error spreads. U2 frees a counter in two places: FreeCounter
 * (0xf29230) and WaitAndFree (0xf2a040, its own copy of FreeCounter's body), which now calls
 * FreeCounter instead (route_wait_and_free). A freed counter then waits in a FIFO of
 * BB_COUNTER_QUARANTINE (1024; 0: off) before it is really freed, so a late decrement lands on an
 * object nobody owns (U2 in play uses 20-150 of the 2048). A stub in the code segment's zero tail
 * (0x13e3400) takes FreeCounter's entry: a counter already in the ring is not taken twice (U2's
 * navigation queries free theirs twice), otherwise it goes into the ring and the one it pushes out
 * is freed by the rest of FreeCounter. Off with BB_COUNTER_LOG (it hooks the same entry). */
static const unsigned char free_counter_prologue[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
    0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x18, 0x49, 0x89, 0xfc};
static int route_wait_and_free(uint64_t image_size) {
    const uint64_t base = 0x400000, wf = 0xf2a040, at = 0xf2a067;
    /* 0xf2a062: call WaitForCounter; 0xf2a067: the start of its copy of FreeCounter */
    static const unsigned char call_wait[] = {0xe8, 0xa9, 0xfe, 0xff, 0xff};
    static const unsigned char old_tail[] = {0x4c, 0x8d, 0x35, 0x12, 0x42, 0x66, 0x00};
    /* mov rdi, r12; call 0xf29230; jmp 0xf2a219 (the stack guard check and the epilogue) */
    static const unsigned char new_tail[] = {0x4c, 0x89, 0xe7, 0xe8, 0xc1, 0xf1, 0xff, 0xff,
                                             0xe9, 0xa5, 0x01, 0x00, 0x00};
    if (at - base + sizeof(new_tail) > image_size) return 0;
    if (!memcmp(image + at - base, new_tail, sizeof(new_tail))) return 1;
    if (memcmp(image + wf - base, free_counter_prologue, sizeof(free_counter_prologue)) ||
        memcmp(image + 0xf2a062 - base, call_wait, sizeof(call_wait)) ||
        memcmp(image + at - base, old_tail, sizeof(old_tail)))
        return 0;
    memcpy(image + at - base, new_tail, sizeof(new_tail));
    return 1;
}
static struct { volatile uint32_t next, pad; volatile uint64_t ring[1024]; } counter_quarantine;
static void install_counter_quarantine(uint64_t image_size) {
    const char *env = getenv("BB_COUNTER_QUARANTINE");
    long n = env ? strtol(env, NULL, 10) : 1024;
    if (n <= 0) return;
    if (n > 1024) n = 1024;
    uint32_t size = 16;
    while (size < (uint32_t)n) size <<= 1;
    const uint64_t base = 0x400000, cave = 0x13e3400 - base, entry = 0xf29230;
    if (cave + 128 > image_size || entry - base + sizeof(free_counter_prologue) > image_size)
        return;
    if (memcmp(image + entry - base, free_counter_prologue, sizeof(free_counter_prologue))) {
        if (image[entry - base] == 0xe9)
            printf("Counter quarantine: FreeCounter is hooked already (BB_COUNTER_LOG), off\n");
        return; /* not U2 1.02 */
    }
    for (size_t i = 0; i < 128; ++i)
        if (image[cave + i]) { printf("Counter quarantine: the code cave is not empty, off\n"); return; }
    if (!route_wait_and_free(image_size)) {
        printf("Counter quarantine: WaitAndFree is not U2's, off\n");
        return;
    }
    unsigned char *t = image + cave;
    size_t k = 0;
    const uint64_t state = (uint64_t)(uintptr_t)&counter_quarantine;
    const uint32_t mask = size - 1;
#define B(...) do { const unsigned char b_[] = {__VA_ARGS__}; memcpy(t + k, b_, sizeof(b_)); k += sizeof(b_); } while (0)
    B(0x48, 0xb8); memcpy(t + k, &state, 8); k += 8;       /* movabs rax, state */
    B(0x31, 0xc9);                                          /* xor ecx, ecx */
    const size_t loop = k;
    B(0x48, 0x39, 0x7c, 0xc8, 0x08);                        /* cmp [rax+rcx*8+8], rdi */
    B(0x74); const size_t je_dup = k; B(0x00);              /* je ret: in the ring already */
    B(0xff, 0xc1);                                          /* inc ecx */
    B(0x81, 0xf9); memcpy(t + k, &size, 4); k += 4;         /* cmp ecx, size */
    B(0x72, (unsigned char)(loop - (k + 2)));               /* jb loop */
    B(0xb9, 0x01, 0x00, 0x00, 0x00);                        /* mov ecx, 1 */
    B(0xf0, 0x0f, 0xc1, 0x08);                              /* lock xadd [rax], ecx */
    B(0x81, 0xe1); memcpy(t + k, &mask, 4); k += 4;         /* and ecx, mask */
    B(0x48, 0x87, 0x7c, 0xc8, 0x08);                        /* xchg rdi, [rax+rcx*8+8] */
    B(0x48, 0x85, 0xff);                                    /* test rdi, rdi */
    B(0x74, 0x0b);                                          /* jz ret: the ring was not full yet */
    B(0x55, 0x48, 0x89, 0xe5, 0x41, 0x57);                  /* FreeCounter's moved prologue */
    const int32_t back = (int32_t)((int64_t)(entry + 6) - (int64_t)(base + cave + k + 5));
    B(0xe9); memcpy(t + k, &back, 4); k += 4;               /* jmp 0xf29236: frees the one out */
    t[je_dup] = (unsigned char)(k - (je_dup + 1));
    B(0xc3);                                                /* ret */
#undef B
    unsigned char *p = image + entry - base;
    const int32_t r = (int32_t)((int64_t)(base + cave) - (int64_t)(entry + 5));
    p[0] = 0xe9; memcpy(p + 1, &r, 4); p[5] = 0x90;         /* FreeCounter: jmp stub; nop */
    printf("Counter quarantine: freed job counters wait in a FIFO of %u before the pool reuses "
           "them (%zu bytes of stub at %#" PRIx64 ")\n", size, k, base + cave);
}
/* Naughty Dog's job manager (all three games): the six NdJobWorkerThreads spin on their queues
 * with `pause` unless the manager's sleep flag (byte +0x59, set by its own SetSleepMode) is on.
 * With it on, an idle worker waits on the manager's condition variable after 1000 spins (10 ms
 * timeout) and every job submission wakes them. On the PS4 each worker owns a core and the flag
 * stays off; on a 4-core CPU (Steam Deck) the spinning workers starve the GPU command thread.
 * The flag is treated as on in the worker loop, in its re-check before waiting and in the submit
 * path: all three or none. Found by BilalReloaded
 * (github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port, JOB_SLEEP_PATCHES in
 * tools/probe_uncharted.py) on 01.09. The retail 01.02 (the EU dump) has the same three sites:
 * its worker loop is identical, but its re-check loads the manager through r13 (not r15) and its
 * submit path tests rdi and jumps with a short je. A build that matches no set keeps spinning.
 * BB_JOB_SLEEP=0 keeps the spinning workers. */
static void patch_job_sleep(uint64_t image_size) {
    const char *env=getenv("BB_JOB_SLEEP");
    if (env && env[0]=='0') { printf("Job workers: spin while idle (BB_JOB_SLEEP=0)\n"); return; }
#ifdef __ANDROID__
    /* g2-arm64: off unless BB_JOB_SLEEP=1. On a phone's CPU the sleeping workers start jobs late
     * enough to outlive the frame memory they use: U3's particle jobs crashed within minutes,
     * U2's navmesh queries and U1's loading end failed too (android/notes/findings.md, 2026-10-21). */
    if (!env) { printf("Job workers: spin while idle (BB_JOB_SLEEP=1 lets them sleep)\n"); return; }
#endif
#ifdef _WIN32
    /* Sleeping workers free the cores of small CPUs, but cost ~8% in busy scenes on a 16-thread
     * one (U3 Cartagena square: 87 vs 94 FPS). By default only up to 8 hardware threads;
     * BB_JOB_SLEEP=1 forces it. */
    const DWORD threads=GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (!env && threads>8) { printf("Job workers: spin while idle (%lu hardware threads)\n",(unsigned long)threads); return; }
#endif
    struct Patch {
        const char *name, *pattern;
        unsigned offset, original_size, size;
        unsigned char original[2], replacement[6];
    };
    static const struct Patch builds[][3]={
        {
            /* je past the sleep -> nop nop */
            {"worker loop","4d8d7001 807f5900 74?? 4981f8e8030000 7c??",8,1,2,{0x74},{0x90,0x90}},
            {"re-check before waiting","498b07 80785900 74?? 4889c7 4883ef80",7,1,2,{0x74},{0x90,0x90}},
            /* je rel32 past the wake-up -> 6-byte nop */
            {"job submission wakes them","80785900 0f84????0000 4883c060",4,2,6,{0x0f,0x84},
             {0x66,0x0f,0x1f,0x44,0x00,0x00}},
        },
        {
            /* 01.02: same sites, r13 in the re-check and a short je through rdi in the submit */
            {"worker loop","4d8d7001 807f5900 74?? 4981f8e8030000 7c??",8,1,2,{0x74},{0x90,0x90}},
            {"re-check before waiting","498b4500 80785900 74?? 4889c7 4883ef80",8,1,2,{0x74},{0x90,0x90}},
            {"job submission wakes them","807f5900 74?? 4883c760 beffffffff",4,1,2,{0x74},{0x90,0x90}},
        },
    };
    const size_t nbuilds=sizeof(builds)/sizeof(builds[0]);
    for (size_t b=0;b<nbuilds;++b) {
        int64_t at[3];
        size_t i;
        for (i=0;i<3;++i) {
            at[i]=find_unique_pattern(image_size,builds[b][i].pattern);
            if (at[i]<0 || memcmp(image+at[i]+builds[b][i].offset,builds[b][i].original,
                                  builds[b][i].original_size))
                break;
        }
        if (i<3) {
            if (b+1<nbuilds) continue;
            printf("Job workers: %s not found, idle workers keep spinning\n",builds[b][i].name);
            return;
        }
        for (i=0;i<3;++i) {
            memcpy(image+at[i]+builds[b][i].offset,builds[b][i].replacement,builds[b][i].size);
            printf("Job workers: %s at %#" PRIx64 " (idle workers sleep)\n",builds[b][i].name,
                   (uint64_t)at[i]+builds[b][i].offset);
        }
        return;
    }
}
/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. */
void runtime_restart(void) {
    fflush(NULL);
#ifdef _WIN32
    /* run.py sets BB_RESTART_COMMAND: the launch command line, started again (new patches). */
    const char *command=getenv("BB_RESTART_COMMAND");
    puts("Runtime: restarting through run.py");
    if (!command || !*command) { fputs("runtime_restart: BB_RESTART_COMMAND is not set\n",stderr); _exit(1); }
    char *line=_strdup(command);
    /* Same output handles: a launcher reading the game's log keeps reading the new launch. */
    STARTUPINFOA startup={.cb=sizeof(startup),.dwFlags=STARTF_USESTDHANDLES,
                          .hStdInput=GetStdHandle(STD_INPUT_HANDLE),.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE),
                          .hStdError=GetStdHandle(STD_ERROR_HANDLE)};
    PROCESS_INFORMATION process;
    if (!line || !CreateProcessA(NULL,line,NULL,NULL,TRUE,0,NULL,NULL,&startup,&process)) {
        fprintf(stderr,"runtime_restart: CreateProcess failed (%lu)\n",GetLastError()); _exit(1);
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    /* The new launch waits for this process's GPU device to go away (run.py --after). */
    TerminateProcess(GetCurrentProcess(),0);
#elif defined(BB_SDL_MAIN)
    /* g2-arm64: the Android app restarts itself: GameActivity.restartApp() starts a helper
     * activity in a process of its own, which starts the game again once this process is gone
     * (its GPU device and memory with it). GameActivity then boots what BB_LOADEXEC_FILE asks
     * for (sceSystemServiceLoadExec), or files/args.txt. */
    puts("Runtime: restarting the app");
    fflush(NULL);
    JNIEnv *env=(JNIEnv *)SDL_GetAndroidJNIEnv();
    jobject activity=(jobject)SDL_GetAndroidActivity();
    jmethodID restart=NULL;
    if (env && activity) {
        jclass cls=(*env)->GetObjectClass(env,activity);
        restart=cls ? (*env)->GetMethodID(env,cls,"restartApp","()V") : NULL;
        if (restart) (*env)->CallVoidMethod(env,activity,restart);
        if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); restart=NULL; }
    }
    if (!restart) fputs("runtime_restart: GameActivity.restartApp failed: start the game again by hand\n",stderr);
    fflush(NULL);
    _exit(0);
#else
    puts("Runtime: restarting through run.sh");
    syscall(SYS_close_range, 3u, ~0u, 0u);
    execlp("bash", "bash", "run.sh", (char *)NULL);
    perror("runtime_restart: exec");
    _exit(1);
#endif
}

int main(int argc, char **argv) {
#ifndef _WIN32
    /* --log <file> (g2-arm64: an Android app has no console): stdout and stderr go there. */
    for (int i = 2; i + 1 < argc; ++i)
        if (!strcmp(argv[i], "--log")) {
            if (!freopen(argv[i+1], "w", stdout)) return 1;
            dup2(fileno(stdout), 2);
            for (int j = i; j + 2 <= argc; ++j) argv[j] = argv[j+2];
            argc -= 2;
            break;
        }
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
#ifndef _WIN32
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits. */
#ifdef __GLIBC__
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
#endif
    /* (ARM64 hosts have a 39/48-bit address space: host pointers fit in 40 bits.) */
    if (guest_cpu_init()) fail("guest CPU backend initialization failed");
#endif
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
    if (argc == 2 && !strcmp(argv[1], "--controller-info")) return runtime_pad_tool(0,100);
    if (argc == 3 && !strcmp(argv[1], "--controller-test"))
        return runtime_pad_tool(1,atoi(argv[2]));
    check_cpu();
#ifdef _WIN32
    /* The PS4 user range must stay free of host allocations (GPU driver, heaps), and the guest
     * TCB needs one of the 64 TEB TLS slots before DLLs (Vulkan drivers) take them. */
    runtime_memory_reserve();
    runtime_thread_tcb_offset();
#endif
    int cpu_only = 0, strict_imports = 0, list_missing = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL, *guest_argv_file=NULL, *trophy_dir=NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--list-missing")) list_missing = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches") && i+1<argc) patch_file=argv[++i];
        else if (!strcmp(argv[i], "--guest-argv") && i+1<argc) guest_argv_file=argv[++i];
        else if (!strcmp(argv[i], "--trophies") && i+1<argc) trophy_dir=argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [options] | --vulkan-only | --controller-info | --controller-test 0..100\n", argv[0]);
        return 1;
    }
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"BBCONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
        char serial[16]="", title[128]="", version[16]="";
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"APP_VER",version,sizeof(version),NULL);
        bbnet_configure(app0,serial,title,version);
    }
    if (trophy_dir) runtime_trophy_configure(trophy_dir);
    bbgpu_register_kernel();
#ifdef _WIN32
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
    AddVectoredExceptionHandler(1, vectored_fault);
    SetUnhandledExceptionFilter(unhandled_fault);
    if (getenv("BB_STRICT_HANDLES")) {
        PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY strict={0};
        strict.RaiseExceptionOnInvalidHandleReference=1;
        strict.HandleExceptionsPermanentlyEnabled=1;
        if (!SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy,&strict,sizeof(strict)))
            fprintf(stderr,"BB_STRICT_HANDLES: policy not set (%lu)\n",GetLastError());
    }
    (void)timeout_seconds; /* no watchdog on Windows */
#else
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGTRAP, &sa, NULL);
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(timeout_seconds); /* 0 disables the watchdog */
    if (getenv("BB_DUMP_REQUEST") && *getenv("BB_DUMP_REQUEST")) {
        pthread_t dump_thread;
        if (pthread_create(&dump_thread,NULL,dump_on_request,(void *)getenv("BB_DUMP_REQUEST"))==0)
            pthread_detach(dump_thread);
    }
    if (getenv("BB_PEEK_FILE") && *getenv("BB_PEEK_FILE")) {
        pthread_t peek_thread;
        if (pthread_create(&peek_thread,NULL,peek_on_request,(void *)getenv("BB_PEEK_FILE"))==0)
            pthread_detach(peek_thread);
    }
#endif
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "BBPROBE1", 8) && memcmp(magic, "BBPROBE2", 8) && memcmp(magic,"BBPROBE3",8) && memcmp(magic,"BBPROBE4",8) && memcmp(magic,"BBPROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "BBPROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"BBPROBE5",8);
    int linked=!memcmp(magic,"BBPROBE3",8) || !memcmp(magic,"BBPROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* BBPROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"BBPROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    if (list_missing) {
        unsigned char *kinds=calloc(import_count ? import_count : 1,1);
        if (!kinds) fail("allocation failed");
        for (uint64_t i=0;i<nr;++i) if (relocs[i].kind) kinds[relocs[i].value]=(unsigned char)relocs[i].kind;
        uint64_t missing=0;
        for (uint64_t i=0;i<import_count;++i) {
            if (!kinds[i] || runtime_resolve(names[i],kinds[i]==2) || (native_libc && bindings[i])) continue;
            const char *symbol=runtime_symbol(names[i]);
            printf("missing %s %s%s\n",names[i],symbol ? symbol : "?",kinds[i]==2 ? " (data)" : "");
            ++missing;
        }
        printf("%" PRIu64 " of %" PRIu64 " imports missing\n",missing,import_count);
        return missing ? 20 : 0;
    }
    image = runtime_image_map(round_page(size), PROT_READ | PROT_WRITE);
    if (!image) image = allocate(round_page(size));
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (!cpu_only) {
        char title[128]="Bloodborne", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        BbGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (bbgpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,bbgpu_symbol_count());
#ifndef _WIN32
        { const char *env=getenv("BB_STALL_REPORT_SECONDS");
          const long limit=env ? strtol(env,NULL,10) : 10;
          pthread_t stall_thread;
          if (limit>0 && pthread_create(&stall_thread,NULL,stall_watch,(void *)(intptr_t)limit)==0) pthread_detach(stall_thread); }
#endif
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        /* SysV: mov edi, index; movabs rax, handler; jmp rax. No fake return values. */
        uint32_t index = (uint32_t)i;
        uintptr_t handler = guest_host_function((const void *)unresolved, "unresolved import", GUEST_ABI_SYSV);
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; /* mov rsi,rdi: preserve arg0 */
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
            if (resolved && relocs[i].kind == 1) value = guest_host_function((const void *)resolved, runtime_import_name(names[relocs[i].value]), GUEST_ABI_SYSV);
            else if (resolved) value = resolved + relocs[i].addend;
            else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
            }
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
    install_guest_hooks(segments, ns);
    {
        uint64_t image_end=0;
        for (uint64_t s=0;s<ns;++s) if (segments[s].address+segments[s].size>image_end) image_end=segments[s].address+segments[s].size;
        install_game_callbacks(segments, ns, image_end);
        patch_eop_tick_wait(image_end);
        patch_frame_params_wait(image_end);
        patch_job_sleep(image_end);
        install_counter_log(image_end);
        install_counter_quarantine(image_end);
        runtime_display_patch(image, image_end);
    }
#ifdef _WIN32
    printf("Guest TCB loads redirected to the TEB TLS slot: %" PRIu64 "\n", patch_tcb_loads(segments, ns));
    /* Last code patch: Windows exception frames and the red zone of leaf functions (red_zone.c). */
    {
        RedZoneSegment *loaded=calloc(ns ? ns : 1,sizeof(*loaded));
        if (!loaded) fail("allocation failed");
        for (uint64_t s=0;s<ns;++s) loaded[s]=(RedZoneSegment){segments[s].address,segments[s].size,segments[s].flags};
        red_zone_patch(image, size, loaded, ns);
        free(loaded);
    }
#endif
#if defined(__x86_64__)
    /* g2-arm64: on ARM64 FEXCore runs every instruction the game uses. */
    {
        uint64_t code[64][2];
        size_t n=0;
        for (uint64_t s=0;s<ns;++s)
            if (segments[s].flags&1) { code[n][0]=segments[s].address; code[n][1]=segments[s].size; ++n; }
        cpu_patch_image(image, size, (const uint64_t (*)[2])code, n);
    }
#endif
    protect(traps, round_page((import_count + 1) * 32), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    printf("Guest image at %p\n", (void *)image);
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        runtime_set_guest_fpu();
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            int result=(int)GUEST_CALL(image+modules[m].init,0,0,0);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
#ifdef _WIN32
    if (getenv("BB_WATCH")) CloseHandle(CreateThread(NULL,0,watch_thread,getenv("BB_WATCH"),0,NULL));
#endif
    /* argv: the executable and, after sceSystemServiceLoadExec, the arguments it was given
     * (--guest-argv: one per line, the guest path first). The caller's list is argv as is, its
     * first element argv[0] (execve): the collection menu passes "eboot.bin"
     * "ispartofuccollection", and U2 took an "eboot.bin" in argv[1] for an option and stayed
     * on a grey screen. The path is argv[0] only when no arguments were given. */
    struct { uint64_t argc; const char *argv[34]; } params = {1, {"/app0/eboot.bin", NULL}};
    if (guest_argv_file) {
        FILE *list=fopen(guest_argv_file,"rb");
        static char lines[33][1024];
        uint64_t n=0;
        while (list && n<33 && fgets(lines[n],sizeof(lines[n]),list)) {
            lines[n][strcspn(lines[n],"\r\n")]=0;
            params.argv[n]=lines[n]; ++n;
        }
        if (list) fclose(list);
        if (n>1) {
            for (uint64_t i=1;i<n;++i) params.argv[i-1]=lines[i];
            params.argc=n-1; params.argv[n-1]=NULL;
        } else if (n) { params.argc=1; params.argv[1]=NULL; }
        printf("Guest argv: %s (+%" PRIu64 " arguments)\n",params.argv[0],params.argc-1);
    }
    const uint64_t exit_callback=guest_host_function((const void *)guest_exit,"guest_exit",GUEST_ABI_SYSV);
#ifdef _WIN32
    GUEST_CALL(image + entry, (uint64_t)(uintptr_t)&params, exit_callback);
#else
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,PROT_READ|PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
    guest_call_on_stack((uintptr_t)(image+entry),(const uint64_t[]){(uint64_t)(uintptr_t)&params,exit_callback},2,
                        stack+MAIN_STACK-64);
#endif
    fail("entry unexpectedly returned");
}
