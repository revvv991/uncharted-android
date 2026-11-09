/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
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
    runtime_quit(20); /* no destructors: GPU, audio and guest threads are still running */
}
void runtime_quit(int code) {
    static volatile long quitting;
    fflush(NULL);
    if (!__atomic_exchange_n(&quitting,1,__ATOMIC_ACQ_REL) && gpu_enabled) bbgpu_prepare_exit();
    fflush(NULL);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(),(UINT)code);
#endif
    _exit(code);
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
/* An address in committed executable memory (a return address candidate). */
static int executable_address(uintptr_t address) {
    MEMORY_BASIC_INFORMATION m;
    if (address<0x10000 || !VirtualQuery((const void *)address,&m,sizeof(m)) || m.State!=MEM_COMMIT) return 0;
    return (m.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))!=0;
}
/* After the report: the stack as a list of addresses in executable memory (return address
 * candidates, not a verified call chain: rbp is not a frame pointer in drivers and optimised
 * host code, and a call to a bad pointer leaves only [rsp]). Bounded by the TEB stack (fibers
 * set it to their own) and by what can be read. */
static void scan_stack(uintptr_t sp) {
    uintptr_t top, limit; /* NT_TIB StackBase, StackLimit (gs:[8], gs:[16]; NtCurrentTeb trips -Warray-bounds) */
    __asm__ volatile("mov %%gs:8,%0\n mov %%gs:16,%1" : "=r"(top), "=r"(limit));
    if (sp<limit || sp>=top || top-sp>32768) top=sp+32768;
    char where[MAX_PATH+64];
    uintptr_t last=0;
    int shown=0;
    for (uintptr_t at=sp&~(uintptr_t)7; at<top && shown<24; at+=8) {
        uint64_t value;
        SIZE_T got=0;
        if (!ReadProcessMemory(GetCurrentProcess(),(const void *)at,&value,sizeof(value),&got) || got!=sizeof(value)) break;
        if (value==last || !executable_address((uintptr_t)value)) continue;
        describe_address(where,sizeof(where),(uintptr_t)value);
        fprintf(stderr,"  stack+0x%04" PRIxPTR " %s\n",at-sp,where);
        last=value; ++shown;
    }
}
/* user/crash.dmp (threads, modules, stacks and the memory they point at): a later crash can be
 * read with a debugger. dbghelp is loaded at start (not in a crashed process); the dump is
 * written by a helper thread that gets 15 s: a lock held by the faulting code (heap, loader)
 * then costs the dump, never a hung process. BB_NO_CRASH_DUMP=1: none. */
typedef BOOL (WINAPI *MiniDumpWriteDumpFn)(HANDLE,DWORD,HANDLE,int,const void *,const void *,const void *);
static MiniDumpWriteDumpFn crash_dump_write;
static char crash_dump_path[MAX_PATH];
static struct { DWORD thread; EXCEPTION_POINTERS *pointers; BOOL client; } crash_exception;
static void crash_dump_configure(const char *user) {
    if (getenv("BB_NO_CRASH_DUMP")) return;
    snprintf(crash_dump_path,sizeof(crash_dump_path),"%s\\crash.dmp",user);
    HMODULE dbghelp=LoadLibraryA("dbghelp.dll");
    if (dbghelp) crash_dump_write=(MiniDumpWriteDumpFn)(void *)GetProcAddress(dbghelp,"MiniDumpWriteDump");
}
static DWORD WINAPI crash_dump_thread(void *unused) {
    (void)unused;
    HANDLE file=CreateFileA(crash_dump_path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file==INVALID_HANDLE_VALUE) return 1;
    /* MiniDumpWithIndirectlyReferencedMemory | WithUnloadedModules | WithProcessThreadData |
     * WithThreadInfo: no full memory (the guest maps gigabytes). */
    const BOOL ok=crash_dump_write(GetCurrentProcess(),GetCurrentProcessId(),file,0x40|0x20|0x100|0x1000,&crash_exception,NULL,NULL);
    CloseHandle(file);
    return ok ? 0 : 2;
}
static void write_crash_dump(EXCEPTION_POINTERS *info) {
    if (!crash_dump_write) return;
    crash_exception.thread=GetCurrentThreadId(); crash_exception.pointers=info; crash_exception.client=FALSE;
    HANDLE helper=CreateThread(NULL,0,crash_dump_thread,NULL,0,NULL);
    DWORD code=3;
    if (helper && WaitForSingleObject(helper,15000)==WAIT_OBJECT_0) GetExitCodeThread(helper,&code);
    if (code==0) fprintf(stderr,"Crash dump: %s\n",crash_dump_path);
    else fprintf(stderr,"Crash dump: not written (%s)\n",code==1 ? "cannot create the file" : code==2 ? "dbghelp failed" : "timed out");
}
static int fatal_reported;
static LONG fatal_exception(EXCEPTION_POINTERS *info) {
    if (__atomic_exchange_n(&fatal_reported,1,__ATOMIC_ACQ_REL)) { Sleep(INFINITE); }
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    const CONTEXT *c=info->ContextRecord;
    char where[MAX_PATH+64];
    describe_address(where,sizeof(where),(uintptr_t)c->Rip);
    char thread[96];
    const char *guest=runtime_thread_name_here(), *fiber=runtime_fiber_name_here();
    if (fiber) snprintf(thread,sizeof(thread),"thread %lu '%s', fiber '%s'",GetCurrentThreadId(),guest ? guest : "?",fiber);
    else if (guest) snprintf(thread,sizeof(thread),"thread %lu '%s'",GetCurrentThreadId(),guest);
    else snprintf(thread,sizeof(thread),"thread %lu, host",GetCurrentThreadId());
    if (record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2)
        fprintf(stderr,"Fault: %s access to %p at %s (%s)\n",
                record->ExceptionInformation[0]==1 ? "write" : record->ExceptionInformation[0]==8 ? "execute" : "read",
                (void *)record->ExceptionInformation[1],where,thread);
    else
        fprintf(stderr,"Fault: exception 0x%08lx at %s (%s)\n",record->ExceptionCode,where,thread);
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
    {
        uint64_t slot;
        SIZE_T got=0;
        if (ReadProcessMemory(GetCurrentProcess(),(const void *)(uintptr_t)c->Rsp,&slot,sizeof(slot),&got) && got==sizeof(slot)) {
            describe_address(where,sizeof(where),(uintptr_t)slot);
            fprintf(stderr,"  [rsp] %s%s\n",where,executable_address((uintptr_t)slot) ? "" : " (not code)");
        }
    }
    scan_stack((uintptr_t)c->Rsp);
    fflush(NULL);
    write_crash_dump(info);
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
    /* g2-arm64: a fault in translated code: the guest instruction and its registers, the frames. */
    { static char live[8192]; int f=guest_cpu_describe_fault(live,sizeof(live),context);
      if (f>0) { ssize_t w_=write(2,live,(size_t)f); (void)w_; } }
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
static volatile uint32_t u1_set_job_skips, u1_set_maker_null; /* install_u1_set_job_guard below */
static volatile uint32_t u3_particle_job_skips; /* install_u3_particle_job_guard below */
/* g2-arm64: patch_job_lock_wait's waits in place that got their counter (count, time, the longest,
 * in us), those that suspended after the full wait, the counter waits that reached it on workers
 * (with a render context open or not), and the render contexts opened (the Begin callback in
 * runtime_pad.c): the peek thread reports them every 10 s. */
static volatile uint32_t job_lock_waits, job_lock_wait_max_us, job_lock_suspends, job_lock_tries, job_lock_tries_open;
static volatile uint64_t job_lock_wait_total_us;
volatile uint32_t probe_ndgi_begins;
/* install_dt_smooth below: nominal (s, 0 = off), debt, k, correction low/high, debt low/high,
 * the last raw dt - read and written by the guest stub. */
static float dt_state[8];
static int dt_installed, dt_fixed; /* the stub is in; BB_DT_SMOOTH=<ms> fixed the nominal */
/* g2-arm64: the present thread's frame time on the screen (s; 0: frames not paced, movies), the
 * nominal of the games' frame dt smoothing (install_dt_smooth); the debt starts over when it turns on. */
void runtime_frame_time_hint(float seconds) {
    if (!dt_installed || dt_fixed || dt_state[0] == seconds) return;
    if (dt_state[0] == 0.0f) dt_state[1] = 0.0f;
    dt_state[3] = -seconds / 4;
    dt_state[4] = seconds;
    dt_state[0] = seconds;
    printf("Frame dt smoothing: %.3f ms a frame%s\n", seconds * 1e3, seconds > 0.0f ? "" : " (off: frames not paced)");
}
static void *peek_on_request(void *arg) {
    const char *path=(const char *)arg;
    char out[512], part[512];
    snprintf(out,sizeof(out),"%s.out",path);
    snprintf(part,sizeof(part),"%s.part",path);
    static unsigned char data[1<<20];
    uint32_t skips_told=0, particle_skips_told=0;
    unsigned ticks=0;
    int contexts_told=0;
    for (;;) {
        usleep(200000);
        /* g2-arm64: and the U1 set job guard's counts, when they grow. */
        const uint32_t skips=__atomic_load_n(&u1_set_job_skips,__ATOMIC_RELAXED);
        const uint32_t nulls=__atomic_load_n(&u1_set_maker_null,__ATOMIC_RELAXED);
        if (skips+nulls!=skips_told) {
            printf("Set job guard: %u of U1's node set jobs skipped with no set; the set maker left "
                   "it null %u times\n",skips,nulls);
            fflush(stdout);
            skips_told=skips+nulls;
        }
        /* g2-arm64: and U3's particle job guard's skips, when they grow. */
        const uint32_t particle_skips=__atomic_load_n(&u3_particle_job_skips,__ATOMIC_RELAXED);
        if (particle_skips!=particle_skips_told) {
            printf("Particle job guard: %u of U3's particle draw registration jobs skipped with no data\n",
                   particle_skips);
            fflush(stdout);
            particle_skips_told=particle_skips;
        }
        /* g2-arm64: and the job wait's waits every 10 s when there were any (the render contexts
         * once, to show that its callbacks run). */
        if (++ticks%50==0) {
            const uint32_t waits=__atomic_exchange_n(&job_lock_waits,0,__ATOMIC_RELAXED);
            const uint32_t suspended=__atomic_exchange_n(&job_lock_suspends,0,__ATOMIC_RELAXED);
            const uint64_t total=__atomic_exchange_n(&job_lock_wait_total_us,0,__ATOMIC_RELAXED);
            const uint32_t longest=__atomic_exchange_n(&job_lock_wait_max_us,0,__ATOMIC_RELAXED);
            const uint32_t contexts=__atomic_exchange_n(&probe_ndgi_begins,0,__ATOMIC_RELAXED);
            const uint32_t tries=__atomic_exchange_n(&job_lock_tries,0,__ATOMIC_RELAXED);
            const uint32_t tries_open=__atomic_exchange_n(&job_lock_tries_open,0,__ATOMIC_RELAXED);
            if (waits || suspended || tries_open || (contexts && !contexts_told)) {
                printf("Job wait: %u waits in place in 10 s (%.1f ms in all, the longest %.1f ms), %u "
                       "suspended after the full wait; %u counter waits on workers, %u with a render "
                       "context open; %u render contexts opened\n",waits,total/1000.0,longest/1000.0,
                       suspended,tries,tries_open,contexts);
                fflush(stdout);
                contexts_told=1;
            }
        }
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
/* g2-arm64 (U1's job fiber crash, findings 2026-11-01): U1's game frame (0x49c5b0) reads the main
 * view's region table in 0x739b90 (per tracked entity the bitset of its region; the crash is its
 * movbe at 0x739ed2). The table and its bitsets live in the render frame arena (allocator 0x36):
 * the render frame job (0x4f0cb0) resets that arena at 0x4f0dfb and rebuilds the table only later,
 * in its view update (0x97dab0 -> 0x9a4260 -> 0x9bba20: the old table freed, the pointer zeroed, a
 * new table allocated, published at once and then filled). In between the published table is
 * memory the job's other allocations reuse; nothing orders the game frame's read against that, and
 * on the PS4 the read evidently never lands there, here it does (the watch of 2026-11-01: job
 * entries, counter pointers or floats in the published table for 1-30 ms every 10-40 s; 4 crashes
 * in ~140 U1 runs). Now the arena reset sets a stale flag in host memory and the end of the view
 * update clears it; while it is set the reader takes no bitset (as for a null one: the entity
 * counts as in view), nor for a pointer outside the guest heap (0x739e66); and a spin lock covers
 * the reader's loop (no calls but a math function, no fiber switch) and the whole of 0x9bba20
 * (allocations and asserts only, entered from four places) for its zeroed pointer.
 * BB_U1_VIEW_LOCK=0: off. */
static struct { volatile uint32_t lock, reader_held, stale; } u1_view_lock;
static int u1_view_lock_on;
static void install_u1_view_lock(uint64_t image_size) {
    const char *env = getenv("BB_U1_VIEW_LOCK");
    if (env && env[0] == '0') return;
    const uint64_t base = 0x400000, cave = 0xd1fe00, acquire_at = 0x739d3b, release_at = 0x73a0ac,
                   bits_at = 0x739e66, producer = 0x9bba20, reset_at = 0x4f0df5, views_at = 0x4f0e81,
                   views = 0x97dab0;
    static const unsigned char acquire_old[] = {0x4d, 0x8d, 0xb5, 0x60, 0x03, 0x00, 0x00};
    static const unsigned char release_old[] = {0x48, 0x8b, 0x45, 0x90, 0xa8, 0x01};
    static const unsigned char bits_old[] = {0x48, 0x8b, 0x44, 0x10, 0x20, 0x48, 0x89, 0x45, 0x88};
    static const unsigned char producer_old[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    static const unsigned char reset_old[] = {0x48, 0x8b, 0x08, 0x48, 0x89, 0xc7, 0xff, 0x51, 0x18};
    static const unsigned char views_old[] = {0xe8, 0x2a, 0xcc, 0x48, 0x00};
    if (cave - base + 512 > image_size) return;
    if (memcmp(image + acquire_at - base, acquire_old, sizeof(acquire_old)) ||
        memcmp(image + release_at - base, release_old, sizeof(release_old)) ||
        memcmp(image + bits_at - base, bits_old, sizeof(bits_old)) ||
        memcmp(image + producer - base, producer_old, sizeof(producer_old)) ||
        memcmp(image + reset_at - base, reset_old, sizeof(reset_old)) ||
        memcmp(image + views_at - base, views_old, sizeof(views_old)))
        return; /* not U1 */
    for (size_t i = 0; i < 512; ++i)
        if (image[cave - base + i]) { printf("View table lock: the code cave is not empty, off\n"); return; }
    unsigned char *t = image + cave - base;
    size_t k = 0;
    const uint64_t state = (uint64_t)(uintptr_t)&u1_view_lock;
    const uint32_t tries = 20000; /* pause is wfe here (~100 us): a guard of ~2 s, never met */
#define B(...) do { const unsigned char b_[] = {__VA_ARGS__}; memcpy(t + k, b_, sizeof(b_)); k += sizeof(b_); } while (0)
#define STATE_R11() do { B(0x49, 0xbb); memcpy(t + k, &state, 8); k += 8; } while (0) /* movabs r11, state */
    /* A, before the reader's loop: the moved lea, then the lock (reader_held when taken) */
    const uint64_t a = cave + k;
    B(0x4d, 0x8d, 0xb5, 0x60, 0x03, 0x00, 0x00);            /* lea r14, [r13+0x360] */
    STATE_R11();
    B(0xb9); memcpy(t + k, &tries, 4); k += 4;              /* mov ecx, tries */
    const size_t a_spin = k;
    B(0xb8, 0x01, 0x00, 0x00, 0x00);                        /* mov eax, 1 */
    B(0x41, 0x87, 0x03);                                    /* xchg [r11], eax */
    B(0x85, 0xc0);                                          /* test eax, eax */
    B(0x74); const size_t a_got = k; B(0x00);               /* je got */
    B(0xf3, 0x90);                                          /* pause */
    B(0xff, 0xc9);                                          /* dec ecx */
    B(0x75, (unsigned char)(a_spin - (k + 2)));             /* jne spin */
    B(0x41, 0xc7, 0x43, 0x04, 0x00, 0x00, 0x00, 0x00);      /* mov dword [r11+4], 0 */
    B(0xc3);                                                /* ret */
    t[a_got] = (unsigned char)(k - (a_got + 1));
    B(0x41, 0xc7, 0x43, 0x04, 0x01, 0x00, 0x00, 0x00);      /* got: mov dword [r11+4], 1 */
    B(0xc3);
    /* R, after the loop: the lock released when held, then the moved mov and test */
    const uint64_t r = cave + k;
    STATE_R11();
    B(0x41, 0x83, 0x7b, 0x04, 0x00);                        /* cmp dword [r11+4], 0 */
    B(0x74); const size_t r_skip = k; B(0x00);              /* je skip */
    B(0x41, 0xc7, 0x43, 0x04, 0x00, 0x00, 0x00, 0x00);      /* mov dword [r11+4], 0 */
    B(0x41, 0xc7, 0x03, 0x00, 0x00, 0x00, 0x00);            /* mov dword [r11], 0 */
    t[r_skip] = (unsigned char)(k - (r_skip + 1));
    B(0x48, 0x8b, 0x45, 0x90);                              /* skip: mov rax, [rbp-0x70] */
    B(0xa8, 0x01);                                          /* test al, 1 */
    B(0xc3);
    /* G, the bitset pointer: none while the table is stale or outside [0x10_0000_0000,
     * 0x40_0000_0000) (r11 holds the loop's mask: rdx is the scratch here) */
    const uint64_t g = cave + k;
    B(0x48, 0x8b, 0x44, 0x10, 0x20);                        /* mov rax, [rax+rdx+0x20] */
    B(0x48, 0xba); memcpy(t + k, &state, 8); k += 8;        /* movabs rdx, state */
    B(0x83, 0x7a, 0x08, 0x00);                              /* cmp dword [rdx+8], 0 */
    B(0x75); const size_t g_none = k; B(0x00);              /* jne none */
    B(0x48, 0x89, 0xc2);                                    /* mov rdx, rax */
    B(0x48, 0xc1, 0xea, 0x24);                              /* shr rdx, 36 */
    B(0x48, 0xff, 0xca);                                    /* dec rdx */
    B(0x48, 0x83, 0xfa, 0x02);                              /* cmp rdx, 2 */
    B(0x76, 0x02);                                          /* jbe ok */
    t[g_none] = (unsigned char)(k - (g_none + 1));
    B(0x31, 0xc0);                                          /* none: xor eax, eax */
    B(0x48, 0x89, 0x45, 0x88);                              /* ok: mov [rbp-0x78], rax */
    B(0xc3);
    /* P, the rebuild's entry: the lock, the function through T, the lock released when taken */
    const uint64_t p = cave + k;
    B(0x48, 0x83, 0xec, 0x08);                              /* sub rsp, 8 */
    STATE_R11();
    B(0x41, 0xba); memcpy(t + k, &tries, 4); k += 4;        /* mov r10d, tries */
    const size_t p_spin = k;
    B(0xb8, 0x01, 0x00, 0x00, 0x00);                        /* mov eax, 1 */
    B(0x41, 0x87, 0x03);                                    /* xchg [r11], eax */
    B(0x85, 0xc0);                                          /* test eax, eax */
    B(0x74); const size_t p_got = k; B(0x00);               /* je got */
    B(0xf3, 0x90);                                          /* pause */
    B(0x41, 0xff, 0xca);                                    /* dec r10d */
    B(0x75, (unsigned char)(p_spin - (k + 2)));             /* jne spin */
    B(0xc7, 0x04, 0x24, 0x00, 0x00, 0x00, 0x00);            /* mov dword [rsp], 0 */
    B(0xeb); const size_t p_go = k; B(0x00);                /* jmp call */
    t[p_got] = (unsigned char)(k - (p_got + 1));
    B(0xc7, 0x04, 0x24, 0x01, 0x00, 0x00, 0x00);            /* got: mov dword [rsp], 1 */
    t[p_go] = (unsigned char)(k - (p_go + 1));
    B(0xe8); const size_t p_call = k; k += 4;               /* call T */
    B(0x83, 0x3c, 0x24, 0x00);                              /* cmp dword [rsp], 0 */
    B(0x74); const size_t p_out = k; B(0x00);               /* je out */
    STATE_R11();
    B(0x41, 0xc7, 0x03, 0x00, 0x00, 0x00, 0x00);            /* mov dword [r11], 0 */
    t[p_out] = (unsigned char)(k - (p_out + 1));
    B(0x48, 0x83, 0xc4, 0x08);                              /* out: add rsp, 8 */
    B(0xc3);                                                /* ret (rax: the function's result) */
    /* T: the moved prologue, then the rest of 0x9bba20 */
    const uint64_t tr = cave + k;
    { const int32_t rel = (int32_t)((int64_t)tr - (int64_t)(cave + p_call + 4)); memcpy(t + p_call, &rel, 4); }
    B(0x55, 0x48, 0x89, 0xe5, 0x41, 0x57);                  /* push rbp; mov rbp, rsp; push r15 */
    B(0xe9);
    { const int32_t rel = (int32_t)((int64_t)(producer + 6) - (int64_t)(cave + k + 4)); memcpy(t + k, &rel, 4); k += 4; }
    /* S, the render job's arena reset: stale, then the moved virtual call */
    const uint64_t sr = cave + k;
    STATE_R11();
    B(0x41, 0xc7, 0x43, 0x08, 0x01, 0x00, 0x00, 0x00);      /* mov dword [r11+8], 1 */
    B(0x48, 0x8b, 0x08);                                    /* mov rcx, [rax] */
    B(0x48, 0x89, 0xc7);                                    /* mov rdi, rax */
    B(0x48, 0x83, 0xec, 0x08);                              /* sub rsp, 8 */
    B(0xff, 0x51, 0x18);                                    /* call [rcx+0x18] */
    B(0x48, 0x83, 0xc4, 0x08);                              /* add rsp, 8 */
    B(0xc3);                                                /* ret */
    /* V, the render job's view update: the call, then no longer stale */
    const uint64_t vu = cave + k;
    B(0x48, 0x83, 0xec, 0x08);                              /* sub rsp, 8 */
    B(0xe8);
    { const int32_t rel = (int32_t)((int64_t)views - (int64_t)(cave + k + 4)); memcpy(t + k, &rel, 4); k += 4; }
    B(0x48, 0x83, 0xc4, 0x08);                              /* add rsp, 8 */
    STATE_R11();
    B(0x41, 0xc7, 0x43, 0x08, 0x00, 0x00, 0x00, 0x00);      /* mov dword [r11+8], 0 */
    B(0xc3);                                                /* ret */
#undef STATE_R11
#undef B
    /* the six sites: call A; call R; call G; jmp P; call S; call V */
    struct { uint64_t at, to; size_t size; unsigned char op; } sites[] = {
        {acquire_at, a, sizeof(acquire_old), 0xe8}, {release_at, r, sizeof(release_old), 0xe8},
        {bits_at, g, sizeof(bits_old), 0xe8}, {producer, p, sizeof(producer_old), 0xe9},
        {reset_at, sr, sizeof(reset_old), 0xe8}, {views_at, vu, sizeof(views_old), 0xe8}};
    for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
        unsigned char *s = image + sites[i].at - base;
        const int32_t rel = (int32_t)((int64_t)sites[i].to - (int64_t)(sites[i].at + 5));
        s[0] = sites[i].op;
        memcpy(s + 1, &rel, 4);
        memset(s + 5, 0x90, sites[i].size - 5);
    }
    u1_view_lock_on = 1;
    printf("View table lock: U1's view region table read only while valid (%zu bytes of stubs at %#" PRIx64 ")\n",
           k, cave);
}

/* g2-arm64 (U1's second job crash, findings 2026-11-01): U1's job 0xa16aa0 takes 0x50 bytes of
 * data from its two submitters (0x9fb520, 0x9fbe4b: a scene object, checked by its tag 0x610ba10,
 * and that object's +0x78, a set of up to 1024 nodes - two 16-word bitmasks and the nodes - which
 * the job copies and walks, marking the nodes and visiting their children). It faulted at 0xa16b04
 * reading +0x20 of a null set (viewfix-u1-B4, one of ~40 U1 runs that day): the object's +0x78 was
 * empty when the job was submitted, and the submitters do not check it. The job now returns at its
 * entry when its set is null, as for an empty set (the job runner then completes it as usual); the
 * peek thread reports the skips. The set is made by the job 0xa1f000 (submitted at 0x9fa8da with the
 * object as its data): after pushing a scoped allocator of its own (0x8585a0), 128 KiB for +0x70 and
 * 32 KiB for +0x78 from the thread's top allocator - null for both when that stack is empty; its
 * null stores of the set (0xa1f12e) are counted too. BB_U1_SET_GUARD=0: off. */
static void install_u1_set_job_guard(uint64_t image_size) {
    const char *env = getenv("BB_U1_SET_GUARD");
    if (env && env[0] == '0') return;
    const uint64_t base = 0x400000, cave = 0xd20000, job = 0xa16aa0, load_at = 0xa16b00,
                   store_at = 0xa1f12e;
    static const unsigned char job_old[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                            0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x18, 0x01, 0x00, 0x00};
    static const unsigned char load_old[] = {0x49, 0x8b, 0x42, 0x08, 0x48, 0x8b, 0x48, 0x20};
    static const unsigned char store_old[] = {0x49, 0x89, 0x5c, 0x24, 0x78, 0x49, 0xc7, 0x84, 0x24,
                                              0x88, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00};
    if (cave - base + 128 > image_size) return;
    if (memcmp(image + job - base, job_old, sizeof(job_old)) ||
        memcmp(image + load_at - base, load_old, sizeof(load_old)) ||
        memcmp(image + store_at - base, store_old, sizeof(store_old)))
        return; /* not U1 */
    for (size_t i = 0; i < 128; ++i)
        if (image[cave - base + i]) { printf("Set job guard: the code cave is not empty, off\n"); return; }
    unsigned char *t = image + cave - base;
    size_t k = 0;
    const uint64_t skips = (uint64_t)(uintptr_t)&u1_set_job_skips;
#define B(...) do { const unsigned char b_[] = {__VA_ARGS__}; memcpy(t + k, b_, sizeof(b_)); k += sizeof(b_); } while (0)
    B(0x48, 0x83, 0x7f, 0x08, 0x00);                        /* cmp qword [rdi+8], 0: the set */
    B(0x74); const size_t skip = k; B(0x00);                /* je skip */
    B(0x55);                                                /* push rbp (the moved prologue) */
    B(0x48, 0x89, 0xe5);                                    /* mov rbp, rsp */
    B(0x41, 0x57);                                          /* push r15 */
    B(0xe9);                                                /* jmp job + 6 */
    { const int32_t rel = (int32_t)((int64_t)(job + 6) - (int64_t)(cave + k + 4)); memcpy(t + k, &rel, 4); k += 4; }
    t[skip] = (unsigned char)(k - (skip + 1));
    B(0x48, 0xb8); memcpy(t + k, &skips, 8); k += 8;        /* skip: movabs rax, &skips */
    B(0xf0, 0xff, 0x00);                                    /* lock inc dword [rax] */
    B(0xc3);                                                /* ret */
    /* The maker's store of the set, at +64 (rax is dead there: the next load overwrites it). */
    const size_t m = 64;
    k = m;
    const uint64_t nulls = (uint64_t)(uintptr_t)&u1_set_maker_null;
    B(0x49, 0x89, 0x5c, 0x24, 0x78);                        /* mov [r12+0x78], rbx: the set */
    B(0x48, 0x85, 0xdb);                                    /* test rbx, rbx */
    B(0x75); const size_t made = k; B(0x00);                /* jne made */
    B(0x48, 0xb8); memcpy(t + k, &nulls, 8); k += 8;        /* movabs rax, &nulls */
    B(0xf0, 0xff, 0x00);                                    /* lock inc dword [rax] */
    t[made] = (unsigned char)(k - (made + 1));
    B(0xc3);                                                /* made: ret */
#undef B
    unsigned char *site = image + job - base;               /* jmp the job's stub */
    int32_t rel = (int32_t)((int64_t)cave - (int64_t)(job + 5));
    site[0] = 0xe9;
    memcpy(site + 1, &rel, 4);
    site[5] = 0x90;
    site = image + store_at - base;                         /* call the store's stub */
    rel = (int32_t)((int64_t)(cave + m) - (int64_t)(store_at + 5));
    site[0] = 0xe8;
    memcpy(site + 1, &rel, 4);
    printf("Set job guard: U1's node set job skips a null set (%zu bytes of stubs at %#" PRIx64 ")\n", k, cave);
}

/* g2-arm64 (the games' jumps, findings 2026-11-02 and 2026-11-03): each game moves its world every
 * frame by the time since the last frame began, through a frame clock that clamps the raw time and
 * stores it (U1: 0x4fac70 into 0xe66f10+0x8; U2: 0x8ca5c0 into 0x1516180+0x4; U3: 0x14ff5c0, called
 * by 0x960690, into 0x1bb4a90+0xc); every game clock is advanced by it. On PS4 the frames begin
 * evenly, held by the GPU's vsync-locked frame ends; here they follow the port's frame ends three
 * frames back, 12-70 ms apart while the screen shows every frame 33.3 ms: the picture moved by the
 * game's steps, not by the screen's (in U1 a recording's frame-to-frame shift correlated 0.91 with
 * the game's frame intervals; U2 moved the same way, 364 steps of a refresh's worth a minute).
 * A stub replaces that store:
 *   out = nominal + clamp(0.15 * (debt + raw - nominal), -nominal/4, +nominal),
 *   debt = clamp(debt + raw - out, -100 ms, +250 ms)
 * so the game time follows real time with a bounded debt and moves by the screen's steps. The
 * nominal is the paced frame's time on the screen from the present thread (runtime_frame_time_hint;
 * 0, the raw dt, when frames are not paced and during movies). On in U1 and U3, opt-in in U2:
 * BB_DT_SMOOTH=0 off, =1 on (U2 too), =<ms> that nominal always (BB_U1_DT_SMOOTH, its first name,
 * still works for U1). 60 s screen recordings while the camera turned, 30 FPS either way: U1
 * (2026-11-02) steps of a refresh's worth or more off the screen's time 418 a minute -> 56, the
 * game's dt p5-p95 15-59 ms -> 31-37 ms, the debt within -23..+44 ms (p1, p99); U3 (2026-11-03, 2
 * pairs) 456 and 468 -> 142 and 134 a minute, the picture's step p5-p95 0.37-2.09 -> 0.59-1.46
 * refreshes' worth, its dt 30-38 ms while the raw one ran 11-48. U2's clock follows the stub as well, yet its picture moved as
 * unevenly (499/499 -> 512/511 a minute, 2 pairs): what moves its view is timed elsewhere (open), so
 * U2 stays as it was unless asked. The sites are found by their bytes, each unique in its game; the
 * stub uses rax, xmm2 and xmm3, dead at all three stores (the next instruction loads rax, xmm2 is
 * loaded or zeroed before it is read, xmm3 is not read before the next call). */
static unsigned char *near_image_memory(size_t size);
static int64_t find_unique_pattern(uint64_t image_size, const char *pattern);
static void install_dt_smooth(uint64_t image_size) {
    struct Site {
        const char *game, *pattern;
        unsigned offset, store_size;
        unsigned char store[6];
        int opt_in; /* only with BB_DT_SMOOTH=1 or =<ms> */
    };
    static const struct Site sites[] = {
        /* vmulsd disp(%rax); vcvtsd2ss; vminss %xmm0,%xmm2,%xmm0; [vmovss %xmm0,0x8(%rbx)]; lea rax */
        {"U1", "c5fb5940?? c5fb5ac0 c5ea5dc0 c5fa114308 488d05", 13, 5, {0xc5, 0xfa, 0x11, 0x43, 0x08}, 0},
        /* vmulsd disp(%rax); vcvtsd2ss; vminss %xmm0,%xmm1,%xmm0; [vmovss %xmm0,0x4(%r15)]; lea rax */
        {"U2", "c5fb5940?? c5fb5ac0 c5f25dc0 c4c17a114704 488d05", 13, 6, {0xc4, 0xc1, 0x7a, 0x11, 0x47, 0x04}, 1},
        /* vdivss 0x20(%r15),%xmm1,%xmm2; vminss %xmm0,%xmm2,%xmm0; [vmovss %xmm0,0xc(%r15)]; lea rax */
        {"U3", "c4c1725e5720 c5ea5dc0 c4c17a11470c 488d05", 10, 6, {0xc4, 0xc1, 0x7a, 0x11, 0x47, 0x0c}, 0},
    };
    static const unsigned char stub[] = {
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,        /* movabs $state,%rax */
        0xc5, 0xfa, 0x10, 0x10,                    /* vmovss (%rax),%xmm2: nominal */
        0xc5, 0xe0, 0x57, 0xdb,                    /* vxorps %xmm3,%xmm3,%xmm3 */
        0xc5, 0xf8, 0x2e, 0xd3,                    /* vucomiss %xmm3,%xmm2 */
        0x76, 0x3d,                                /* jbe store: off */
        0xc5, 0xfa, 0x11, 0x40, 0x1c,              /* vmovss %xmm0,0x1c(%rax): the raw dt */
        0xc5, 0xfa, 0x58, 0x58, 0x04,              /* vaddss 0x4(%rax),%xmm0,%xmm3: raw + debt */
        0xc5, 0xe2, 0x5c, 0xda,                    /* vsubss %xmm2,%xmm3,%xmm3: - nominal */
        0xc5, 0xe2, 0x59, 0x58, 0x08,              /* vmulss 0x8(%rax),%xmm3,%xmm3: * k */
        0xc5, 0xe2, 0x5f, 0x58, 0x0c,              /* vmaxss 0xc(%rax),%xmm3,%xmm3 */
        0xc5, 0xe2, 0x5d, 0x58, 0x10,              /* vminss 0x10(%rax),%xmm3,%xmm3 */
        0xc5, 0xe2, 0x58, 0xda,                    /* vaddss %xmm2,%xmm3,%xmm3: out */
        0xc5, 0xfa, 0x58, 0x50, 0x04,              /* vaddss 0x4(%rax),%xmm0,%xmm2: raw + debt */
        0xc5, 0xea, 0x5c, 0xd3,                    /* vsubss %xmm3,%xmm2,%xmm2: - out */
        0xc5, 0xea, 0x5f, 0x50, 0x14,              /* vmaxss 0x14(%rax),%xmm2,%xmm2 */
        0xc5, 0xea, 0x5d, 0x50, 0x18,              /* vminss 0x18(%rax),%xmm2,%xmm2 */
        0xc5, 0xfa, 0x11, 0x50, 0x04,              /* vmovss %xmm2,0x4(%rax): debt */
        0xc5, 0xf8, 0x28, 0xc3};                   /* vmovaps %xmm3,%xmm0 */
                                                   /* store: the game's own store, then jmp back */
    const struct Site *s = NULL;
    unsigned char *p = NULL;
    for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]) && !s; ++i) {
        const int64_t at = find_unique_pattern(image_size, sites[i].pattern);
        if (at >= 0) { s = &sites[i]; p = image + at + s->offset; }
    }
    if (!s) return; /* another build: no smoothing */
    const char *env = getenv("BB_DT_SMOOTH");
    if (!env && s == &sites[0]) env = getenv("BB_U1_DT_SMOOTH");
    if (env && env[0] == '0' && !env[1]) { printf("Frame dt smoothing: off (BB_DT_SMOOTH=0)\n"); return; }
    if (!env && s->opt_in) { printf("Frame dt smoothing: off in %s (BB_DT_SMOOTH=1 turns it on)\n", s->game); return; }
    unsigned char *t = near_image_memory(256);
    if (!t) { printf("Frame dt smoothing: no thunk memory, off\n"); return; }
    const double nominal_ms = env && strcmp(env, "1") ? atof(env) : 0.0;
    const float nominal = (float)(nominal_ms > 0.0 ? nominal_ms / 1000.0 : 0.0);
    dt_fixed = nominal > 0.0f;
    dt_state[0] = nominal;
    dt_state[1] = 0.0f;
    dt_state[2] = 0.15f;
    dt_state[3] = -nominal / 4;
    dt_state[4] = nominal;
    dt_state[5] = -0.1f;
    dt_state[6] = 0.25f;
    size_t k = sizeof(stub);
    memcpy(t, stub, k);
    const uint64_t state = (uint64_t)(uintptr_t)dt_state;
    memcpy(t + 2, &state, 8);
    memcpy(t + k, s->store, s->store_size);
    k += s->store_size;
    t[k++] = 0xe9;                                                       /* jmp site + store */
    int32_t rel = (int32_t)((int64_t)(uintptr_t)(p + s->store_size) - (int64_t)(uintptr_t)(t + k + 4));
    memcpy(t + k, &rel, 4);
    k += 4;
    rel = (int32_t)((int64_t)(uintptr_t)t - (int64_t)(uintptr_t)(p + 5)); /* the store -> jmp stub */
    p[0] = 0xe9;
    memcpy(p + 1, &rel, 4);
    if (s->store_size > 5) p[5] = 0x90;
    dt_installed = 1;
    printf("Frame dt smoothing: %s's frame clock moves by the screen's frame time with a debt to real "
           "time (%s; %zu bytes of stub)\n", s->game, dt_fixed ? "fixed by BB_DT_SMOOTH" : "from the present thread", k);
}

/* g2-arm64 (U3's particle job crash, findings 2026-11-03): U3's particle draw registration job
 * 0x1388f10 (submitted by DrawRegJob 0x136c9d0, which does not wait for it) copies `data+8`'s fields
 * and faulted at 0x1388f74 reading +0x20 of a null `data+8` - its 0x50 bytes of data come from the
 * single render frame's memory, so a job that runs after that frame's memory is reused reads it
 * empty (2026-10-21/22 with the sleep mode; js-ab-u3-A1 of 2026-11-03 with spinning workers, no
 * frame limit). It has the shape of U1's job 0xa16aa0 byte for byte (the prologue, the load at
 * +0x60) and gets the same guard as 0143's: it returns at its entry when `data+8` is null (its
 * particles go undrawn for that frame instead of the fault; the job runner completes it as usual),
 * the peek thread reports the skips. Garbage that is not null still faults (a job late by more than
 * its frame). BB_U3_PARTICLE_GUARD=0: off. */
static void install_u3_particle_job_guard(uint64_t image_size) {
    const char *env = getenv("BB_U3_PARTICLE_GUARD");
    if (env && env[0] == '0') return;
    const uint64_t base = 0x400000, job = 0x1388f10, load_at = 0x1388f70;
    static const unsigned char job_old[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                            0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x18, 0x01, 0x00, 0x00};
    static const unsigned char load_old[] = {0x49, 0x8b, 0x42, 0x08, 0x48, 0x8b, 0x48, 0x20};
    if (load_at - base + sizeof(load_old) > image_size ||
        memcmp(image + job - base, job_old, sizeof(job_old)) ||
        memcmp(image + load_at - base, load_old, sizeof(load_old)))
        return; /* not U3 */
    unsigned char *t = near_image_memory(64);
    if (!t) { printf("Particle job guard: no thunk memory, off\n"); return; }
    unsigned char *entry = image + job - base;
    size_t k = 0;
    const uint64_t skips = (uint64_t)(uintptr_t)&u3_particle_job_skips;
#define B(...) do { const unsigned char b_[] = {__VA_ARGS__}; memcpy(t + k, b_, sizeof(b_)); k += sizeof(b_); } while (0)
    B(0x48, 0x83, 0x7f, 0x08, 0x00);                        /* cmp qword [rdi+8], 0: the data's +8 */
    B(0x74); const size_t skip = k; B(0x00);                /* je skip */
    B(0x55);                                                /* push rbp (the moved prologue) */
    B(0x48, 0x89, 0xe5);                                    /* mov rbp, rsp */
    B(0x41, 0x57);                                          /* push r15 */
    B(0xe9);                                                /* jmp job + 6 */
    { const int32_t rel = (int32_t)((int64_t)(uintptr_t)(entry + 6) - (int64_t)(uintptr_t)(t + k + 4)); memcpy(t + k, &rel, 4); k += 4; }
    t[skip] = (unsigned char)(k - (skip + 1));
    B(0x48, 0xb8); memcpy(t + k, &skips, 8); k += 8;        /* skip: movabs rax, &skips */
    B(0xf0, 0xff, 0x00);                                    /* lock inc dword [rax] */
    B(0xc3);                                                /* ret */
#undef B
    const int32_t rel = (int32_t)((int64_t)(uintptr_t)t - (int64_t)(uintptr_t)(entry + 5));
    entry[0] = 0xe9;                                        /* jmp the stub */
    memcpy(entry + 1, &rel, 4);
    entry[5] = 0x90;
    printf("Particle job guard: U3's particle draw registration job skips a null data+8 (%zu bytes of stub)\n", k);
}

/* g2-arm64 diagnosis (U1's job fiber crash, 2026-11-01): BB_U1_VIEW_WATCH=1 watches the table
 * U1's 0x739b90 takes its bitsets from (the faulting movbe at 0x739ed2): the view manager at
 * 0x140b4c0, its level data ([+0x10]: the region count at +0x350, read under the rwlock at +0x360)
 * and its view ([+0x40]: the table of 48-byte region entries at +0x10; bit 1 of an entry's +0xc
 * says it has a bitset, its pointer at +0x20). Every millisecond: changes of those four values, and
 * entries below the count with bitset words (+0xc >> 2) whose pointer is not a guest heap address,
 * the table read under the view table lock above, and not while it is stale, when that is
 * installed (as the game's reader). */
static int peek_guest(void *out, uint64_t address, size_t size) {
    struct iovec local={out,size}, remote={(void *)(uintptr_t)address,size};
    return process_vm_readv(getpid(),&local,1,&remote,1,0)==(ssize_t)size;
}
/* g2-arm64 diagnosis (U1's jumps, findings 2026-11-02): BB_U1_FRAME_WATCH=1 polls U1's render
 * frame number every 0.5 ms - FrameBegin's (ndlib/render/fg-draw-mgr.cpp, 0x8d2741: [[0x14e25a8]],
 * or [[0x14e25a0]] by a flag of the caller) - and prints each new frame: the time (CLOCK_MONOTONIC
 * ms), which pointer, the frame, and the GPU end-of-pipe ticks (m_gfxEopTick, +0xa08 of the frame
 * params: 32 slots of 0xa50 bytes at 0x14cdb40) of the frame three back - the one FrameBegin
 * recycles, waiting for its tick (patch_eop_tick_wait) - and of the frame before. The game's own
 * frame times: the screen showed its picture moving unevenly while the frames went out evenly. */
static void *u1_frame_watch(void *arg) {
    (void)arg;
    static const uint64_t pointers[2] = {0x14e25a8, 0x14e25a0};
    uint64_t last[2] = {0, 0};
    for (;;) {
        usleep(500);
        for (int which = 0; which < 2; ++which) {
            uint64_t ptr = 0, frame = 0, tick3 = 0, tick1 = 0;
            if (!peek_guest(&ptr, pointers[which], 8) || !ptr || !peek_guest(&frame, ptr, 8)) continue;
            if (frame == last[which]) continue;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            peek_guest(&tick3, 0x14cdb40 + ((frame - 3) & 31) * 0xa50 + 0xa08, 8);
            peek_guest(&tick1, 0x14cdb40 + ((frame - 1) & 31) * 0xa50 + 0xa08, 8);
            float dt = 0;
            peek_guest(&dt, 0xe66f18, 4); /* the frame clock's dt (install_dt_smooth) */
            printf("Frame watch: %.3f %c frame %" PRIu64 " (+%" PRId64 "), eop tick of frame-3 %#" PRIx64
                   ", frame-1 %#" PRIx64 "; dt %.2f raw %.2f debt %.2f\n", now.tv_sec * 1e3 + now.tv_nsec / 1e6,
                   which ? 'b' : 'a', frame, (int64_t)(frame - last[which]), tick3, tick1, dt * 1e3,
                   dt_state[7] * 1e3, dt_state[1] * 1e3);
            last[which] = frame;
        }
    }
    return NULL;
}
static void *u1_view_watch(void *arg) {
    (void)arg;
    static unsigned char table[4096*48];
    static const char *const names[4]={"level data","view","region count","region table"};
    uint64_t last[4]={0}, changes[4]={0}, shown[4]={0}, bad=0, unreadable=0;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC,&start);
    double summary=0;
    for (;;) {
        usleep(1000);
        uint64_t head[0x60/8];
        if (!peek_guest(head,0x140b4c0,sizeof(head))) continue;
        const uint64_t level=head[0x10/8], view=head[0x40/8];
        uint32_t count=0; uint64_t tablep=0;
        if (!level || !view || !peek_guest(&count,level+0x350,4) || !peek_guest(&tablep,view+0x10,8)) continue;
        clock_gettime(CLOCK_MONOTONIC,&now);
        const double t=(double)(now.tv_sec-start.tv_sec)+(double)(now.tv_nsec-start.tv_nsec)/1e9;
        const uint64_t values[4]={level,view,count,tablep};
        for (int k=0;k<4;++k) {
            if (values[k]==last[k]) continue;
            changes[k]++;
            if (shown[k]++<40)
                printf("View watch: %.3f s %s %#" PRIx64 " -> %#" PRIx64 " (regions %u, table %#" PRIx64 ")\n",
                       t,names[k],last[k],values[k],count,tablep);
            last[k]=values[k];
        }
        if (t-summary>=5.0) {
            if (summary>0)
                printf("View watch: %.0f s: changes level %" PRIu64 ", view %" PRIu64 ", count %" PRIu64 ", table %" PRIu64
                       "; regions %u, table %#" PRIx64 ", bad entries seen %" PRIu64 "\n",
                       t,changes[0],changes[1],changes[2],changes[3],count,tablep,bad);
            summary=t;
        }
        const uint32_t n=count<4096 ? count : 4096;
        if (!n) continue;
        /* as the game's reader: under the view table lock (when installed), the table read afresh */
        if (u1_view_lock_on) while (__atomic_exchange_n(&u1_view_lock.lock,1u,__ATOMIC_ACQUIRE)) usleep(50);
        const int stale=u1_view_lock_on && __atomic_load_n(&u1_view_lock.stale,__ATOMIC_ACQUIRE);
        int got=!stale && peek_guest(&tablep,view+0x10,8) && tablep && peek_guest(table,tablep,(size_t)n*48);
        if (u1_view_lock_on) __atomic_store_n(&u1_view_lock.lock,0u,__ATOMIC_RELEASE);
        if (stale) continue;
        if (!tablep) continue;
        if (!got) {
            if (unreadable++<10) printf("View watch: %.3f s table %#" PRIx64 " (%u entries) unreadable\n",t,tablep,n);
            continue;
        }
        for (uint32_t i=0;i<n;++i) {
            const unsigned char *e=table+(size_t)i*48;
            uint32_t flags; uint64_t bits;
            memcpy(&flags,e+0xc,4); memcpy(&bits,e+0x20,8);
            if (!(flags&2) || !(flags>>2) || (bits>=0x1000000000ull && bits<0x4000000000ull)) continue;
            ++bad;
            if (bad<=30 || !(bad&(bad-1))) {
                char hex[48*3+1]; int p=0;
                for (int b=0;b<48;++b) p+=snprintf(hex+p,sizeof(hex)-p,"%02x",e[b]);
                printf("View watch: %.3f s bad entry %u of %u (#%" PRIu64 "): flags %#x bitset %#" PRIx64
                       " table %#" PRIx64 ": %s\n",t,i,count,bad,flags,bits,tablep,hex);
            }
        }
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
/* g2-arm64 diagnosis: BB_PATCH_SKIP=<offset>|<start>-<end>,...: the preset's writes at those
 * image offsets (end exclusive) are left out - an A/B of one group of writes without another
 * preset (issue #18: U1's job counter waits, 0x44a000-0x460000). */
static int patch_skipped(uint64_t offset) {
    const char *p=getenv("BB_PATCH_SKIP");
    while (p && *p) {
        char *end;
        uint64_t start=strtoull(p,&end,0), stop;
        if (end==p) break;
        stop=start+1;
        if (*end=='-') {
            const char *q=end+1;
            stop=strtoull(q,&end,0);
            if (end==q) break;
        }
        if (offset>=start && offset<stop) return 1;
        if (*end!=',') break;
        p=end+1;
    }
    return 0;
}
/* BBPATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"BBPATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0, skipped=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        if (patch_skipped(offset)) { ++skipped; continue; }
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
    if (skipped) printf("Patches: %" PRIu64 " writes left out (BB_PATCH_SKIP)\n",skipped);
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
    /* g2-arm64: the same on Linux hosts (FEXCore runs the guest at its own addresses, the image at
     * 0x400000): a free range within rel32 reach after the image. */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
    uint64_t end_of_image=round_page(guest_image_size ? guest_image_size : 1);
    uint64_t base=((uint64_t)(uintptr_t)image+end_of_image+(32u<<20)+0xffff)&~(uint64_t)0xffff;
    for (uint64_t step=0;!memory && step<(1900ull<<20);step+=1u<<16) {
        void *want=(void *)(uintptr_t)(base+step);
        void *got=mmap(want,size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        if (got==MAP_FAILED) continue;
        if (got!=want) { munmap(got,size); continue; } /* a kernel without the flag took it as a hint */
        memory=got;
    }
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
    /* Sized from the table, so no entry is dropped for room (a fixed 32 used to cut the table's end
     * without a word, and 8 KiB of thunks about 25 hooks). */
    const size_t thunk_bytes=(count ? count : 1)*320;
    uint8_t installed[count ? count : 1], skipped[count ? count : 1]; /* g2-arm64: skipped - left out on purpose, not reported */
    uint64_t hook_offsets[count ? count : 1]; size_t hooks_done=0;
    int no_thunks=0;
    memset(installed,0,sizeof(installed)); memset(skipped,0,sizeof(skipped));
    guest_image_size=image_size;
    /* A/B tests: BB_CALLBACKS=0 installs none, BB_CALLBACKS_SKIP=<text> none whose name contains the text */
    const char *off=getenv("BB_CALLBACKS"), *skip=getenv("BB_CALLBACKS_SKIP");
    if (off && off[0]=='0') { printf("Callbacks: disabled (BB_CALLBACKS=0)\n"); return; }
    for (size_t i=0;i<count;++i) {
        const RuntimeGuestCallback *c=&callbacks[i];
        if (skip && *skip && strstr(c->name,skip)) continue;
#if !defined(__x86_64__)
        /* g2-arm64: no mouse camera on the device (its touch pad has its own path) */
        if (c->callback && strstr(c->name,"(mouse)")) { skipped[i]=1; continue; }
#endif
        uint64_t found=0;
        size_t length=0, matches=find_pattern(segments,ns,c->pattern,&found,&length);
        if (matches>1) { printf("Callbacks: %s: %zu matches, not installed\n",c->name,matches); continue; }
        if (!matches) continue;
        const uint64_t offset=found+c->skip;
        int hooked=0;
        for (size_t j=0;j<hooks_done;++j) if (hook_offsets[j]==offset) hooked=1;
        if (hooked) continue;                                          /* an earlier entry took this site */
        hook_offsets[hooks_done++]=offset;
        if (!c->callback) {
            memcpy(image+offset,c->patch,c->moved);
            installed[i]=1;
            printf("Callbacks: %s at %#" PRIx64 " (patch)\n",c->name,offset);
            continue;
        }
        /* g2-arm64: without thunk memory only the callbacks stay out (a return here also left out the
         * code patches further down the table: on Android the move action pool and the NaN resets) */
        if (!thunks && !no_thunks && !(thunks=near_image_memory(thunk_bytes))) {
            no_thunks=1;
            printf("Callbacks: no thunk memory near the image: %s and the callbacks after it not installed (the code patches still are)\n",c->name);
        }
        if (!thunks) continue;
        if (used+320>thunk_bytes) { printf("Callbacks: thunk memory full at %s, the rest not installed\n",c->name); break; }
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
        /* g2-arm64: through FEX's veneer on ARM64 (the function itself on x86-64) */
        const uintptr_t base=(uintptr_t)image, host=guest_host_function((const void *)c->callback,c->name,GUEST_ABI_MS);
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
        if (!seen && !ok && group && !skipped[i]) printf("Callbacks: %s not found\n",callbacks[i].name);
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

/* Uncharted 1 (EU and US 1.02): the rigid-body integrator holds still any body slower than "Sticky Speed
 * (nothing-moves speed)" or "Sticky Omega (nothing-rotates omega)", both 0.1 (EU 0x988674 / 0x988670, the game's own
 * names). A body at rest gains g*dt per step: 0.16 m/s at 60 FPS gets it moving, 0.07 m/s at 144 FPS does not, so
 * above 60 FPS bodies that should tip or drop off a ledge stay put and fall asleep (jet ski river barrels hanging in
 * mid-air). The world step (EU 0x235350, dt in xmm0) scales both thresholds by min(1, 60*dt), so every step behaves
 * as at 60 FPS. BB_U1_STICKY=0 keeps the game's values.
 * The same step also scales "Max Stop Frames" (10) and "Max Crawl Frames" (30): a body that stays that slow for that many
 * steps comes to rest (step counters at body+0x136 / +0x138), 0.17 s / 0.5 s at 60 FPS but 0.07 s / 0.21 s at 144, so
 * objects and ragdolls settled early. They become N / min(1, 60*dt) steps, the same time at any rate. BB_U1_SETTLE=0:
 * the game's counts. */
static float *u1_sticky_omega, *u1_sticky_speed, u1_sticky_omega0, u1_sticky_speed0;
static uint32_t *u1_settle_stop, *u1_settle_crawl, u1_settle_stop0, u1_settle_crawl0;
/* "Artificial Damping V" / "W" (0.9998) multiply the velocities every step: 0.9998^(60*dt) per step is the 60 FPS loss
 * per second at any rate. BB_U1_DAMPING=0: the game's factors. */
static float *u1_damp_v, *u1_damp_w, u1_damp_v0, u1_damp_w0;
static ABI void u1_sticky_step(float dt) {
    if (!(dt>0.0f)) return;
    const float scale=dt*60.0f<1.0f ? dt*60.0f : 1.0f;
    if (u1_sticky_omega) {
        *u1_sticky_omega=u1_sticky_omega0*scale;
        *u1_sticky_speed=u1_sticky_speed0*scale;
    }
    if (u1_settle_stop) {
        *u1_settle_stop=(uint32_t)((float)u1_settle_stop0/scale+0.5f);
        *u1_settle_crawl=(uint32_t)((float)u1_settle_crawl0/scale+0.5f);
    }
    if (u1_damp_w) {
        *u1_damp_v=powf(u1_damp_v0,scale);
        *u1_damp_w=powf(u1_damp_w0,scale);
    }
}
#if !defined(__x86_64__)
/* g2-arm64: the thunk cannot call a host (ARM64) function by its address; the port's bridge
 * (guest_host_function) carries the integer registers, so the step's dt travels as its bits. */
static ABI void u1_sticky_step_bits(uint32_t bits) {
    float dt;
    memcpy(&dt,&bits,sizeof(dt));
    u1_sticky_step(dt);
}
#endif
static void patch_u1_sticky(uint64_t image_size) {
    const char *env=getenv("BB_U1_STICKY"), *settle_env=getenv("BB_U1_SETTLE"), *damping_env=getenv("BB_U1_DAMPING");
    const int sticky=!(env && env[0]=='0'), settle=!(settle_env && settle_env[0]=='0'),
              damping=!(damping_env && damping_env[0]=='0');
    if (!sticky && !settle && !damping) return;
    /* push rbp; mov rbp,rsp; push r14 | push rbx; sub rsp,0x10; mov r14,rsi; vmovss [rbp-0x14],xmm0; mov rbx,rdi;
     * mov dword [rbx+0x14eb4],0 */
    const int64_t at=find_unique_pattern(image_size,"554889e5 4156 53 4883ec10 4989f6 c5fa1145ec 4889fb c783b44e010000000000");
    if (at<0) return; /* not Uncharted 1 */
    /* the integrator's tests: vaddss; lea rax,[omega]; vucomiss xmm0,[rax]; jae; mov qword [r15],0 and
     * vmulss xmm3,xmm3,xmm3; vmulss xmm2,xmm2,xmm3; lea rax,[speed]; vmovss xmm3,[rax]; vmulss xmm3,xmm3,xmm3 */
    const int64_t o=find_unique_pattern(image_size,"c5f258c0 488d05???????? c5f82e00 73?? 49c70700000000");
    const int64_t v=find_unique_pattern(image_size,"c5e259db c5ea59d3 488d05???????? c5fa1018 c5e259db");
    /* the settle test: movzx eax,word [rcx+0x136]; lea rdx,[stop]; cmp eax,[rdx]; ja; movzx eax,word [rcx+0x138];
     * lea rcx,[crawl]; cmp eax,[rcx]; ja (EU 0x1c7b63, US 0x1c80d3) */
    const int64_t c=find_unique_pattern(image_size,"0fb78136010000 488d15???????? 3b02 77?? 0fb78138010000 488d0d???????? 3b01 77");
    /* the artificial damping: vmovss xmm4,[rbp-0x2c]; lea rsi,[W]; vmulss xmm2,xmm4,[rsi] (EU 0x1c1e20, US 0x1c2390)
     * and lea rsi,[V]; vmulss xmm2,xmm4,[rsi]; vmulss xmm0,xmm0,xmm2 (EU 0x1c1e50, US 0x1c23c0) */
    const int64_t dw=find_unique_pattern(image_size,"c5fa1065d4 488d35???????? c5da5916");
    const int64_t dv=find_unique_pattern(image_size,"488d35???????? c5da5916 c5fa59c2");
    unsigned char *t=near_image_memory(512);
    int32_t d;
    if (damping && dw>=0 && dv>=0) {
        memcpy(&d,image+dw+8,4); u1_damp_w=(float *)(image+dw+12+d);
        memcpy(&d,image+dv+3,4); u1_damp_v=(float *)(image+dv+7+d);
        u1_damp_w0=*u1_damp_w; u1_damp_v0=*u1_damp_v;
        if (!(u1_damp_w0>0.9f && u1_damp_w0<=1.0f && u1_damp_v0>0.9f && u1_damp_v0<=1.0f)) u1_damp_w=NULL;
    }
    if (damping && !u1_damp_w) printf("U1 physics: artificial damping not found, not scaled\n");
    if (sticky && o>=0 && v>=0) {
        memcpy(&d,image+o+7,4); u1_sticky_omega=(float *)(image+o+11+d);
        memcpy(&d,image+v+11,4); u1_sticky_speed=(float *)(image+v+15+d);
        u1_sticky_omega0=*u1_sticky_omega; u1_sticky_speed0=*u1_sticky_speed;
    } else if (sticky) {
        printf("U1 physics: sticky thresholds not found, not scaled\n");
    }
    if (settle && c>=0) {
        memcpy(&d,image+c+10,4); u1_settle_stop=(uint32_t *)(image+c+14+d);
        memcpy(&d,image+c+28,4); u1_settle_crawl=(uint32_t *)(image+c+32+d);
        u1_settle_stop0=*u1_settle_stop; u1_settle_crawl0=*u1_settle_crawl;
        if (!u1_settle_stop0 || !u1_settle_crawl0 || u1_settle_stop0>1000 || u1_settle_crawl0>1000) u1_settle_stop=NULL;
    }
    if (settle && !u1_settle_stop) printf("U1 physics: settle frame counts not found, not scaled\n");
    if ((!u1_sticky_omega && !u1_settle_stop && !u1_damp_w) || !t) return;
    unsigned char *p=image+at;
    size_t k=0;
    static const unsigned char save[]={0x57,0x56,0x50,0x51,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53}; /* push rdi rsi rax rcx rdx r8-r11 */
    memcpy(t+k,save,sizeof(save)); k+=sizeof(save);
    /* The world step's float arguments (dt in xmm0, read right after the prologue) and every other XMM register are
     * live here and caller-saved for u1_sticky_step: sub rsp,0x100; movdqu [rsp+16*n],xmmN for N = 0..15 (13 pushes
     * on the entry's rsp-8 leave rsp 16-byte aligned). */
    static const unsigned char sub_rsp[]={0x48,0x81,0xec,0x00,0x01,0x00,0x00}, add_rsp[]={0x48,0x81,0xc4,0x00,0x01,0x00,0x00};
    memcpy(t+k,sub_rsp,sizeof(sub_rsp)); k+=sizeof(sub_rsp);
    for (int n=0;n<16;++n) {
        const int32_t disp=n*16;
        t[k++]=0xf3; if (n>=8) t[k++]=0x44; t[k++]=0x0f; t[k++]=0x7f;  /* movdqu [rsp+disp32], xmmN */
        t[k++]=(unsigned char)(0x84|((n&7)<<3)); t[k++]=0x24; memcpy(t+k,&disp,4); k+=4;
    }
#if defined(__x86_64__)
    const uintptr_t host=(uintptr_t)u1_sticky_step;      /* the host is x86-64: called with dt in xmm0 */
#else
    /* g2-arm64: through the bridge, dt's bits in edi (the xmm saves above leave xmm0 as it was). */
    const uintptr_t host=guest_host_function((const void *)u1_sticky_step_bits,"u1_sticky_step",GUEST_ABI_SYSV);
    if (!host) return;
    t[k++]=0xc5; t[k++]=0xf9; t[k++]=0x7e; t[k++]=0xc7;  /* vmovd edi, xmm0 */
#endif
    t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&host,8); k+=8; /* movabs rax, u1_sticky_step */
    t[k++]=0xff; t[k++]=0xd0;                            /* call rax: u1_sticky_step(xmm0) */
    for (int n=0;n<16;++n) {
        const int32_t disp=n*16;
        t[k++]=0xf3; if (n>=8) t[k++]=0x44; t[k++]=0x0f; t[k++]=0x6f;  /* movdqu xmmN, [rsp+disp32] */
        t[k++]=(unsigned char)(0x84|((n&7)<<3)); t[k++]=0x24; memcpy(t+k,&disp,4); k+=4;
    }
    memcpy(t+k,add_rsp,sizeof(add_rsp)); k+=sizeof(add_rsp);
    static const unsigned char restore[]={0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5a,0x59,0x58,0x5e,0x5f};
    memcpy(t+k,restore,sizeof(restore)); k+=sizeof(restore);
    memcpy(t+k,p,6); k+=6;                               /* push rbp; mov rbp,rsp; push r14 */
    const uint64_t back=(uint64_t)(uintptr_t)(p+6);
    t[k++]=0xff; t[k++]=0x25; memset(t+k,0,4); k+=4; memcpy(t+k,&back,8); k+=8; /* jmp [rip]: p+6 */
    const int32_t to=(int32_t)((int64_t)(uintptr_t)t-(int64_t)(uintptr_t)(p+5));
    p[0]=0xe9; memcpy(p+1,&to,4); p[5]=0x90;
    if (u1_sticky_omega)
        printf("U1 physics: sticky speed %.3f and omega %.3f scale with the step (world step %#" PRIx64 ")\n",
               u1_sticky_speed0,u1_sticky_omega0,(uint64_t)at);
    if (u1_settle_stop)
        printf("U1 physics: bodies settle after %u / %u steps at 60 FPS, the same time at any rate (counts at %#" PRIx64 ")\n",
               u1_settle_stop0,u1_settle_crawl0,(uint64_t)c);
    if (u1_damp_w)
        printf("U1 physics: artificial damping %.4f / %.4f per 60 FPS step, the same loss per second at any rate\n",
               u1_damp_v0,u1_damp_w0);
}
/* The job system's counter wait (corelib job scheduling: until [counter+0x20] == or <= a target),
 * from Mevlock's dev fork: a job worker that finds the counter not there yet suspends its job
 * (SuspendJob) and retries when the job runs again, which may be on another worker; other threads
 * sleep 1 ms and retry in place. On the PS4 each worker owns a core and the counter is there in
 * time. With 4 cores the jobs it waits for lag: U2's water Draw waits with a render command
 * context open, resumed on another worker and failed "m_workerThreadIndex ==
 * GetCurrentWorkerThreadIndex()" (End) and "cmdBuf.m_bInUse == false" (Begin, on the worker it
 * left). A worker with a render context open (the Begin/End hooks in runtime_pad.c) now first
 * retries in place (SwitchToThread, then 1 ms sleeps, up to BB_JOB_LOCK_WAIT_MS, default 250;
 * 0: off) and suspends its job only after that, as before; other workers suspend at once. The
 * counter is read again on every try. */
extern _Thread_local int runtime_ndgi_open; /* runtime_fiber.c */
static uint64_t job_lock_try;
static unsigned job_lock_wait_ms=250;
static volatile long job_lock_fallbacks;
#if !defined(__x86_64__)
/* g2-arm64: the game's try function is x86 code, so its two tests run here: mode 1 is a signed
 * value <= target (setle), mode 0 value == target (sete, setle with the recipes' robust counter
 * waits, 5fe325c), any other mode fails; job_lock_tests reads which from the function's bytes. */
static uint8_t job_lock_le[2];
static int job_lock_ready(uint64_t value, uint64_t target, uint32_t mode) {
    if (mode>1) return 0;
    return job_lock_le[mode] ? (int64_t)value<=(int64_t)target : value==target;
}
/* cmp edx,1; jne; cmp rdi,rsi; setcc al; ret; test edx,edx; jne; cmp rdi,rsi; setcc al; ret;
 * xor eax,eax; ret - 0 when the function has another shape. */
static int job_lock_tests(const unsigned char *f) {
    static const unsigned char shape[]={0x83,0xfa,0x01,0x75,0x07,0x48,0x39,0xf7,0x0f,0x00,0xc0,0xc3,0x85,
                                        0xd2,0x75,0x07,0x48,0x39,0xf7,0x0f,0x00,0xc0,0xc3,0x31,0xc0,0xc3};
    for (size_t i=0;i<sizeof(shape);++i)
        if (i!=9 && i!=20 && f[i]!=shape[i]) return 0;
    if ((f[9]!=0x9e && f[9]!=0x94) || (f[20]!=0x9e && f[20]!=0x94)) return 0;
    job_lock_le[1]=f[9]==0x9e;
    job_lock_le[0]=f[20]==0x9e;
    return 1;
}
#endif
static ABI int job_lock_wait(uint64_t counter, uint64_t a, uint32_t b) {
    const volatile uint64_t *value=(const volatile uint64_t *)(uintptr_t)(counter+0x20);
#if defined(__x86_64__)
    typedef int (ABI *Try)(uint64_t, uint64_t, uint32_t);
    const Try try_value=(Try)(uintptr_t)job_lock_try;
#define try_lock(x,y) try_value(*value,(x),(y))
#else
#define try_lock(x,y) job_lock_ready(*value,(x),(y))
#endif
#ifndef _WIN32
    __atomic_add_fetch(&job_lock_tries,1,__ATOMIC_RELAXED); /* g2-arm64: the report's counts */
    if (runtime_ndgi_open) __atomic_add_fetch(&job_lock_tries_open,1,__ATOMIC_RELAXED);
#endif
    /* Without a render context open the job may move: suspend at once, as the game does (waiting
     * in place for every counter starved the job system: 3.6 fps loading with 4 cores). */
    if (!runtime_ndgi_open) return 0;
#ifdef _WIN32
    for (unsigned i=0;i<16;++i) {
        if (try_lock(a,b)&0xff) return 1;
        SwitchToThread();
    }
    for (unsigned ms=0;ms<job_lock_wait_ms;++ms) {
        if (try_lock(a,b)&0xff) return 1;
        Sleep(1);
    }
#else
    struct timespec t0, t1; /* g2-arm64: the wait's time, for the peek thread's report */
    clock_gettime(CLOCK_MONOTONIC,&t0);
    for (unsigned i=0;i<job_lock_wait_ms*10;++i) {
        if (try_lock(a,b)&0xff) {
            clock_gettime(CLOCK_MONOTONIC,&t1);
            const uint32_t us=(uint32_t)((t1.tv_sec-t0.tv_sec)*1000000+(t1.tv_nsec-t0.tv_nsec)/1000);
            __atomic_add_fetch(&job_lock_waits,1,__ATOMIC_RELAXED);
            __atomic_add_fetch(&job_lock_wait_total_us,us,__ATOMIC_RELAXED);
            uint32_t longest=__atomic_load_n(&job_lock_wait_max_us,__ATOMIC_RELAXED);
            while (us>longest && !__atomic_compare_exchange_n(&job_lock_wait_max_us,&longest,us,1,
                                                              __ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
            return 1;
        }
        usleep(100);
    }
#endif
    if (try_lock(a,b)&0xff) return 1;
#ifndef _WIN32
    __atomic_add_fetch(&job_lock_suspends,1,__ATOMIC_RELAXED);
#endif
    const long n=__atomic_add_fetch(&job_lock_fallbacks,1,__ATOMIC_RELAXED);
    if (n<=20) printf("Job wait: a worker with a render context open waited %u ms in vain, its job suspends (%ld)\n",
                      job_lock_wait_ms,n);
    return 0;
#undef try_lock
}
/* jmp [rip+0]; dq target */
static size_t jump_abs(unsigned char *t, uint64_t target) {
    t[0]=0xff; t[1]=0x25; memset(t+2,0,4); memcpy(t+6,&target,8);
    return 14;
}
/* US 1.02 (CUSA02320, as CUSA02344), all three games: the try is inlined (counter r15, target r14,
 * mode ebx) and the worker check is in al:
 *   mov rcx,[r15+0x20]; cmp ebx,1; jne; cmp rcx,r14; setle cl; jmp; test ebx,ebx; jne; cmp rcx,r14;
 *   sete cl; test cl,cl; jne done; test al,al; je not_a_worker; nop10; (suspend loop:) mov rdi,r15;
 *   mov rsi,r14; mov edx,ebx; call SuspendJob
 * The game keeps an uncalled copy of the try function, which job_lock_wait calls. */
static void patch_job_lock_wait_us(uint64_t image_size) {
    /* g2-arm64: or with mode 0's sete made setle by the recipes' robust counter waits (5fe325c) */
    int64_t at=find_unique_pattern(image_size, "498b4f20 83fb01 75?? 4c39f1 0f9ec1 eb?? 85db 75?? 4c39f1 0f94c1 84c9 75?? 84c0 74?? "
                                               "662e0f1f840000000000 4c89ff 4c89f6 89da e8????????");
    if (at<0) at=find_unique_pattern(image_size, "498b4f20 83fb01 75?? 4c39f1 0f9ec1 eb?? 85db 75?? 4c39f1 0f9ec1 84c9 75?? 84c0 74?? "
                                                 "662e0f1f840000000000 4c89ff 4c89f6 89da e8????????");
    int64_t try_at=find_unique_pattern(image_size, "83fa01 7507 4839f7 0f9ec0 c3 85d2 7507 4839f7 0f94c0 c3");
    if (try_at<0) try_at=find_unique_pattern(image_size, "83fa01 7507 4839f7 0f9ec0 c3 85d2 7507 4839f7 0f9ec0 c3");
    if (at<0 || try_at<0) { printf("Job wait: pattern not found, not patched\n"); return; }
    unsigned char *p=image+at;
    job_lock_try=(uint64_t)(uintptr_t)(image+try_at);
#if !defined(__x86_64__)
    if (!job_lock_tests(image+try_at)) { printf("Job wait: the try function has another shape, not patched\n"); return; }
#endif
    const uint64_t done=(uint64_t)(uintptr_t)(p+31+(int8_t)p[30]);
    const uint64_t not_worker=(uint64_t)(uintptr_t)(p+35+(int8_t)p[34]);
    const uint64_t suspend_loop=(uint64_t)(uintptr_t)(p+45);
    unsigned char *t=near_image_memory(4096);
    if (!t) { printf("Job wait: no thunk memory, not patched\n"); return; }
    size_t k=0;
    const uintptr_t host=guest_host_function((const void *)job_lock_wait,"job_lock_wait",GUEST_ABI_SYSV); /* g2-arm64 */
    t[k++]=0x84; t[k++]=0xc0; t[k++]=0x75; t[k++]=0x0e;                 /* test al,al; jnz worker */
    k+=jump_abs(t+k,not_worker);                                         /* not a worker */
    static const unsigned char args[]={0x4c,0x89,0xff,0x4c,0x89,0xf6,0x89,0xda}; /* rdi=r15 (the counter), rsi=r14, edx=ebx */
    memcpy(t+k,args,sizeof(args)); k+=sizeof(args);
    t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&host,8); k+=8;                /* movabs rax, job_lock_wait */
    t[k++]=0xff; t[k++]=0xd0;                                           /* call rax (rsp aligned as at the original call) */
    t[k++]=0x84; t[k++]=0xc0;                                           /* test al,al */
    t[k++]=0x74; t[k++]=0x0e;                                           /* jz suspend */
    k+=jump_abs(t+k,done);                                               /* there */
    k+=jump_abs(t+k,suspend_loop);                                       /* suspend as before */
    const int32_t to=(int32_t)((int64_t)(uintptr_t)t-(int64_t)(uintptr_t)(p+31+5));
    p[31]=0xe9; memcpy(p+32,&to,4);                                     /* replaces test al,al; je */
    memset(p+36,0x90,9);
    printf("Job wait: workers with a render context open wait for a job counter in place for up to %u ms before suspending their job (0x%" PRIx64 ", US)\n",
           job_lock_wait_ms,(uint64_t)at);
}
static void patch_job_lock_wait(uint64_t image_size) {
    const char *env=getenv("BB_JOB_LOCK_WAIT_MS");
    if (env) job_lock_wait_ms=(unsigned)strtoul(env,NULL,10);
    if (!job_lock_wait_ms) { printf("Job wait: BB_JOB_LOCK_WAIT_MS=0, workers suspend at once\n"); return; }
#ifdef _WIN32
    /* By default only on CPUs with up to 12 hardware threads, where the crash happens (4 and 6
     * cores). With more, a free worker takes the suspended job at once, and waiting in place cost
     * U3's Cartagena square 92 -> 50 FPS on a big CPU. BB_JOB_LOCK_WAIT_MS turns it on anywhere. */
    const DWORD threads=GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (!env && threads>12) {
        printf("Job wait: workers suspend at once (%lu hardware threads; BB_JOB_LOCK_WAIT_MS=250 to wait)\n",
               (unsigned long)threads);
        return;
    }
#endif
    /* mov rdi,[rbx+0x20]; mov rsi,r15; mov edx,r14d; call try; test al,al; jne done;
     * test r12b,r12b; je not_a_worker; nop5; (suspend loop:) mov rdi,rbx; mov rsi,r15; mov edx,r14d; call SuspendJob */
    const int64_t at=find_unique_pattern(image_size, "488b7b20 4c89fe 4489f2 e8???????? 84c0 75?? 4584e4 74?? 0f1f440000 "
                                                     "4889df 4c89fe 4489f2 e8????????");
    if (at<0) { patch_job_lock_wait_us(image_size); return; }
    unsigned char *p=image+at;
    int32_t rel; memcpy(&rel,p+11,4);
    job_lock_try=(uint64_t)(uintptr_t)(p+15+rel);
#if !defined(__x86_64__)
    if (!job_lock_tests(p+15+rel)) { printf("Job wait: the try function has another shape, not patched\n"); return; }
#endif
    const uint64_t done=(uint64_t)(uintptr_t)(p+19+(int8_t)p[18]);
    const uint64_t not_worker=(uint64_t)(uintptr_t)(p+24+(int8_t)p[23]);
    const uint64_t suspend_loop=(uint64_t)(uintptr_t)(p+29);
    unsigned char *t=near_image_memory(4096);
    if (!t) { printf("Job wait: no thunk memory, not patched\n"); return; }
    size_t k=0;
    const uintptr_t host=guest_host_function((const void *)job_lock_wait,"job_lock_wait",GUEST_ABI_SYSV); /* g2-arm64 */
    static const unsigned char test_r12b[]={0x45,0x84,0xe4,0x75,0x0e};   /* test r12b,r12b; jnz worker */
    memcpy(t+k,test_r12b,sizeof(test_r12b)); k+=sizeof(test_r12b);
    k+=jump_abs(t+k,not_worker);                                         /* not a worker */
    static const unsigned char args[]={0x48,0x89,0xdf,0x90,0x4c,0x89,0xfe,0x44,0x89,0xf2}; /* rdi=rbx (the counter), rsi=r15, edx=r14d */
    memcpy(t+k,args,sizeof(args)); k+=sizeof(args);
    t[k++]=0x48; t[k++]=0xb8; memcpy(t+k,&host,8); k+=8;                /* movabs rax, job_lock_wait */
    t[k++]=0xff; t[k++]=0xd0;                                           /* call rax (rsp aligned as at the original calls) */
    t[k++]=0x84; t[k++]=0xc0;                                           /* test al,al */
    t[k++]=0x74; t[k++]=0x0e;                                           /* jz suspend */
    k+=jump_abs(t+k,done);                                               /* acquired */
    k+=jump_abs(t+k,suspend_loop);                                       /* suspend as before */
    const int32_t to=(int32_t)((int64_t)(uintptr_t)t-(int64_t)(uintptr_t)(p+19+5));
    p[19]=0xe9; memcpy(p+20,&to,4);                                     /* replaces test r12b,r12b; je */
    printf("Job wait: workers with a render context open wait for a job counter in place for up to %u ms before suspending their job (0x%" PRIx64 ")\n",
           job_lock_wait_ms,(uint64_t)at);
}
/* g2-arm64: the job manager's two fiber resume paths bump the ready counters of the workers
 * (+0x2040+worker*8 of the priority level) without waking a worker asleep in the timed wait - the
 * add path locks the manager's mutex (+0x60) and broadcasts its condition variable (+0x80), these
 * two do not (U1, U2 and U3 alike: findings 2026-10-21 and 2026-11-07). With BB_JOB_SPINS the
 * workers are asleep most of the time, so a resumed fiber waited up to BB_JOB_WAIT_US for one.
 * BB_JOB_RESUME_WAKE=1: a thunk after each counter loop broadcasts the condition variable through
 * the game's own import stub (the call of the add path), without the mutex: a worker between its
 * re-check and its wait still sleeps the timeout, as before. The manager: the first path keeps it
 * in its frame ([rbp-0x48], the load the thunk displaces), the second in rdi. The stub is a
 * hypercall into the port, which hands the guest's registers back as they were except rax; the
 * thunk saves the caller-saved ones anyway and keeps the stack as the function left it. */
static void patch_job_resume_wake(uint64_t image_size, int64_t submit_at) {
    const char *env=getenv("BB_JOB_RESUME_WAKE");
#ifdef __ANDROID__
    /* g2-arm64: on by default on Android (=0 off): with 2 spins the chase 29.2 -> 30.1 FPS, every
     * pair +0.8 or more, fewer frames over 40 ms; U1 at the limit 2 x 600 s clean (2026-11-07). */
    if (env && env[0]=='0') return;
#else
    if (!env || env[0]!='1') return;
#endif
    /* the add path's broadcast within 0x200 bytes after its sleep-flag check:
     * mov rdi,[r14]; sub rdi,-0x80; call CondBroadcast */
    static const unsigned char bc[]={0x49,0x8b,0x3e,0x48,0x83,0xef,0x80,0xe8};
    int64_t b=-1;
    for (int64_t i=submit_at;i>=0 && i+12<=(int64_t)image_size && i<submit_at+0x200;++i)
        if (!memcmp(image+i,bc,sizeof(bc))) { b=i; break; }
    if (b<0) { printf("Job resume wake: the add path's broadcast not found, off\n"); return; }
    int32_t rel; memcpy(&rel,image+b+8,4);
    const uint64_t stub=(uint64_t)(uintptr_t)(image+b+12+rel);
    /* resume 1: incq [rsi+rdi*8]; mov rdx,[rax+0x100]; inc rdi; cmp rdi,rdx; jb | mov rbx,[rbp-0x48]; mov rax,[rbx+0x4ed8]; mov rcx,[rbx+0x4ee0] */
    const int64_t s1=find_unique_pattern(image_size,"48ff04fe 488b9000010000 48ffc7 4839d7 72e4 488b5db8 488b83d84e0000 488b8be04e0000");
    /* resume 2: incq [rdx+rsi*8]; mov rcx,[rdi+0x100]; inc rsi; cmp rsi,rcx; jb | mov [rsp+0x78],r8; mov r12,r9; xor eax,eax; mov [rsp+0x38],rax; jmp */
    const int64_t s2=find_unique_pattern(image_size,"48ff04f2 488b8f00010000 48ffc6 4839ce 72e4 4c89442478 4d89cc 31c0 4889442438 e9");
    if (s1<0 || s2<0) { printf("Job resume wake: the resume paths not found (%lld, %lld), off\n",(long long)s1,(long long)s2); return; }
    unsigned char *t=near_image_memory(512);
    if (!t) { printf("Job resume wake: no thunk memory, off\n"); return; }
    size_t k=0;
    uint64_t thunk[2];
#define B(...) do { const unsigned char b_[]={__VA_ARGS__}; memcpy(t+k,b_,sizeof(b_)); k+=sizeof(b_); } while (0)
    for (int n=0;n<2;++n) {
        unsigned char *site=image+(n==0?s1:s2)+19;
        const size_t moved=n==0?11:5;
        thunk[n]=(uint64_t)(uintptr_t)(t+k);
        B(0x55, 0x48,0x89,0xe5);                                            /* push rbp; mov rbp,rsp */
        B(0x50,0x51,0x52,0x56,0x57, 0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53); /* push rax,rcx,rdx,rsi,rdi,r8,r9,r10,r11 */
        B(0x48,0x83,0xe4,0xf0);                                             /* and rsp,-16 */
        if (n==0) B(0x48,0x8b,0x7d,0x00, 0x48,0x8b,0x7f,0xb8);              /* mov rdi,[rbp]; mov rdi,[rdi-0x48]: the manager */
        B(0x48,0x85,0xff);                                                  /* test rdi,rdi */
        B(0x74); const size_t skip=k; B(0x00);                              /* jz skip */
        B(0x48,0x83,0xef,0x80);                                             /* sub rdi,-0x80: the condition variable */
        B(0x48,0xb8); memcpy(t+k,&stub,8); k+=8;                            /* movabs rax, the broadcast stub */
        B(0xff,0xd0);                                                       /* call rax */
        t[skip]=(unsigned char)(k-(skip+1));
        B(0x48,0x8d,0x65,0xb8);                                             /* lea rsp,[rbp-72] */
        B(0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58, 0x5f,0x5e,0x5a,0x59,0x58); /* pop r11,r10,r9,r8,rdi,rsi,rdx,rcx,rax */
        B(0x5d);                                                            /* pop rbp */
        memcpy(t+k,site,moved); k+=moved;                                   /* the displaced instructions */
        k+=jump_abs(t+k,(uint64_t)(uintptr_t)(site+moved));                 /* back */
    }
#undef B
    for (int n=0;n<2;++n) {
        unsigned char *site=image+(n==0?s1:s2)+19;
        const int32_t to=(int32_t)((int64_t)thunk[n]-(int64_t)(uintptr_t)(site+5));
        site[0]=0xe9; memcpy(site+1,&to,4);
        if (n==0) { static const unsigned char nop6[]={0x66,0x0f,0x1f,0x44,0x00,0x00}; memcpy(site+5,nop6,6); }
    }
    printf("Job resume wake: a fiber resume broadcasts the job manager's condition variable (sites %#" PRIx64 ", %#" PRIx64 "; the stub %#" PRIx64 ")\n",
           (uint64_t)(uintptr_t)(image+s1+19),(uint64_t)(uintptr_t)(image+s2+19),stub);
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
    /* g2-arm64: on Android the idle workers sleep in every game. Off at first: on a phone's CPU
     * the sleeping workers started jobs late enough to outlive the frame memory they use - U3's
     * particle jobs crashed within minutes, U2's navmesh queries and U1's loading end failed too
     * (android/notes/findings.md, 2026-10-21/22). Since then the counter waits are robust
     * (5fe325c) and U2's freed counters wait in a quarantine (0109). U1 sleeps since 2026-11-02:
     * its six spinning workers held six of the G2's eight cores, and the worker with the frame's
     * job lost its core for tens of ms - hitches of 70-700 ms about every 15 s at the 30 FPS limit;
     * asleep, 3 frames over 70 ms in 8 runs (none over 100). U3 since 2026-11-03: its museum at the
     * 30 limit, 3 interleaved pairs of 300 s, seconds with a frame over 100 ms 10/11/10 spinning
     * against 1/2/2 asleep, the longest 350-668 against 315-349 ms, no crash or stall in 15 minutes;
     * without the limit (4 pairs of 90 s, one run on each side lost to 0153's particle job crash)
     * 35.5/35.3/35.2 spinning against 35.0/35.7/34.6 asleep, no gain and no loss beyond the noise.
     * U2's recipe makes its workers sleep already (five writes: 2 spins, a 1 ms wait), so its
     * worker loop does not match here. */
#endif
#ifdef _WIN32
    /* Sleeping workers free the cores of small CPUs, but cost ~8% in busy scenes on a 16-thread
     * one (U3 Cartagena square: 87 vs 94 FPS). By default up to 12 hardware threads (6-core CPUs
     * such as the i5-10400 doubled their CPU load without it, fork issue #3);
     * BB_JOB_SLEEP=1 forces it. */
    const DWORD threads=GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (!env && threads>12) { printf("Job workers: spin while idle (%lu hardware threads)\n",(unsigned long)threads); return; }
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
            /* g2-arm64: U2's recipe nopped the worker loop's je and made 1000 spins 2 */
            if (find_unique_pattern(image_size,"4d8d7001 807f5900 9090 4981f802000000 7c??")>=0) {
                printf("Job workers: idle workers sleep (the recipe's writes: 2 spins, a 1 ms wait)\n");
                return;
            }
            printf("Job workers: %s not found, idle workers keep spinning\n",builds[b][i].name);
            return;
        }
        for (i=0;i<3;++i) {
            memcpy(image+at[i]+builds[b][i].offset,builds[b][i].replacement,builds[b][i].size);
            printf("Job workers: %s at %#" PRIx64 " (idle workers sleep)\n",builds[b][i].name,
                   (uint64_t)at[i]+builds[b][i].offset);
        }
        /* g2-arm64: BB_JOB_SPINS=<n> (1..999): an idle worker spins n pauses instead of 1000 before
         * it waits, and the wait times out after BB_JOB_WAIT_US (1000) instead of 10 ms - what U2's
         * recipe does ("2 spins, a 1 ms wait"). A guest pause is a wfe of ~100 us on the G2, so the
         * 1000 spins held each of the six workers' cores for ~100 ms after every job: in U3's market
         * chase the workers sat in that spin 70-90% of the time (the guest profiler, 2026-11-07) and
         * the command thread, which shares the big cores with them, was preempted 68 times a frame
         * (its 23 ms of decoding a frame took 29 ms). The A/Bs of 2026-11-07 (the G2, 4 pairs of 120 s,
         * the limit off) with 2: the chase 25.4 -> 29.0 FPS, every pair +3.3 or more, the p99 frame
         * 64 -> 44 ms, at the 30 limit 27.3 -> 29.7; U1 41.2 -> 43.2 and its frames over 40 ms
         * 620-642 -> 7-12 a run. So 2 by default on Android, as U2's recipe has; BB_JOB_SPINS=0 keeps
         * the game's 1000 and 10 ms. One U1 run at the limit with 2 had a GPU page-fault storm that
         * 75 minutes of the same did not repeat (findings 2026-11-07): BB_JOB_SPINS=0 if it comes back. */
        const char *spins_env=getenv("BB_JOB_SPINS");
#ifdef __ANDROID__
        const long spins=spins_env?strtol(spins_env,NULL,10):2;
#else
        const long spins=spins_env?strtol(spins_env,NULL,10):0;
#endif
        if (spins>0 && spins<1000) {
            const uint32_t n=(uint32_t)spins;
            memcpy(image+at[0]+13,&n,4);                         /* cmp r8, 1000 -> cmp r8, n */
            const char *wait_env=getenv("BB_JOB_WAIT_US");
            long wait_us=wait_env?strtol(wait_env,NULL,10):1000;
            if (wait_us<100 || wait_us>10000) wait_us=1000;
            /* add rax,0x60; mov edx,10000; mov rsi,rax; call CondTimedwait; mov ecx,eax; cmp ecx,ETIMEDOUT */
            const int64_t w=find_unique_pattern(image_size,"4883c060 ba10270000 4889c6 e8???????? 89c1 81f93c000280");
            if (w>=0) { const uint32_t us=(uint32_t)wait_us; memcpy(image+w+5,&us,4); }
            printf("Job workers: %ld spins before the wait (BB_JOB_SPINS); the wait times out after %s\n",
                   spins,w>=0?"":"10 ms (the timed wait not found; BB_JOB_WAIT_US ignored)");
            if (w>=0) printf("Job workers: the idle wait times out after %ld us (BB_JOB_WAIT_US)\n",wait_us);
        }
        patch_job_resume_wake(image_size,at[2]);
        return;
    }
}
/* The engine's job runner copies a descriptor it reaches through the job's record
 * (mov rax, [r10+8]; mov rcx, [rax+0x20]); a job run before its descriptor was set read address 0x20
 * and the game stopped (BilalReloaded's port 0.8.0, its #205 #304 #326, all three games). From
 * Mevlock's dev builds: before the two loads a jump goes to a stub in the function's dead assert path
 * (it keeps its int 0x41 first); a null pointer ends the job at the function's own exit, otherwise the
 * loads run and it jumps back. 1.02 code (US CUSA02320, EU/Russian CUSA02344, EU 1.02): the same bytes
 * in those dumps. Both sites are found by their exact bytes and patched only together, at the distance
 * the jumps were made for (1.00 is compiled differently and is left alone). BB_JOB_GUARD=0: off. */
static void patch_job_descriptor_guard(uint64_t image_size) {
    const char *env=getenv("BB_JOB_GUARD");
    if (env && env[0]=='0') return;
    static const struct {
        const char *game, *loads, *cave;
        unsigned char jump[27], stub[24];
        int64_t distance;
    } builds[]={
        {"U1","49c7424800000000488b05a4b6b80049894210498b4208488b4820","488b05efe6a9004885c07427488d3d2ce91e00488d35c4e7",
         {0x49,0xc7,0x42,0x48,0x00,0x00,0x00,0x00,0x48,0x8b,0x05,0xa4,0xb6,0xb8,0x00,0x49,0x89,0x42,0x10,0xe9,0xff,0x0a,0x00,0x00,0x90,0x90,0x90},
         {0xcd,0x41,0x49,0x8b,0x42,0x08,0x48,0x85,0xc0,0x0f,0x84,0x9a,0xff,0xff,0xff,0x48,0x8b,0x48,0x20,0xe9,0xee,0xf4,0xff,0xff},0xb15},
        {"U2","49c7424800000000488b0514e4940049894210498b4208488b4820","488b05618484004885c07427488d3dfa731800488d35ef71",
         {0x49,0xc7,0x42,0x48,0x00,0x00,0x00,0x00,0x48,0x8b,0x05,0x14,0xe4,0x94,0x00,0x49,0x89,0x42,0x10,0xe9,0xfd,0x0a,0x00,0x00,0x90,0x90,0x90},
         {0xcd,0x41,0x49,0x8b,0x42,0x08,0x48,0x85,0xc0,0x0f,0x84,0x9a,0xff,0xff,0xff,0x48,0x8b,0x48,0x20,0xe9,0xf0,0xf4,0xff,0xff},0xb13},
        {"U3","49c7424800000000488b05c4c3d60049894210498b4208488b4820","488b05418dc6004885c07427488d3d08414f00488d35fd3e",
         {0x49,0xc7,0x42,0x48,0x00,0x00,0x00,0x00,0x48,0x8b,0x05,0xc4,0xc3,0xd6,0x00,0x49,0x89,0x42,0x10,0xe9,0xfd,0x0a,0x00,0x00,0x90,0x90,0x90},
         {0xcd,0x41,0x49,0x8b,0x42,0x08,0x48,0x85,0xc0,0x0f,0x84,0x9a,0xff,0xff,0xff,0x48,0x8b,0x48,0x20,0xe9,0xf0,0xf4,0xff,0xff},0xb13},
    };
    for (size_t b=0;b<sizeof(builds)/sizeof(*builds);++b) {
        const int64_t loads=find_unique_pattern(image_size,builds[b].loads);
        if (loads<0) continue;
        const int64_t cave=find_unique_pattern(image_size,builds[b].cave);
        if (cave-loads!=builds[b].distance) {
            printf("Job descriptor guard: %s 1.02 loads found, its stub site not where expected; not patched\n",builds[b].game);
            return;
        }
        memcpy(image+cave,builds[b].stub,sizeof(builds[b].stub));
        memcpy(image+loads,builds[b].jump,sizeof(builds[b].jump));
        printf("Job descriptor guard: %s 1.02 job runner skips a job without its descriptor (%#" PRIx64 ", stub %#" PRIx64 ")\n",
               builds[b].game,(uint64_t)loads,(uint64_t)cave);
        return;
    }
}
/* U2 chapter 21 (convoy): Elena's truck sinks 2-4 m into the snow for ~0.5 s after a bump. The bump
 * plays an animation on the truck by adding a node to its animation blend (US 0x399700, EU 0x102330:
 * node, anim, params). Adding a node copies the params' bool at +0x11 into bit 2 of the previous
 * node's flags as (flag << 2). The play-animation call the convoy uses (US 0xa21b20) never writes
 * that bool: it is whatever was on the stack. With 0x64 there, 0x64 << 2 = 0x90 also sets bit 4 of
 * the previous node, "the reference frame follows the object", so the rail animation that keeps the
 * truck on its rail now takes the truck's own last position as its base each frame and the bump's
 * offsets pile up (traced: flags 0xc8 -> 0xd8 at each sink, 0x64 at +0x11 each time). PS4 stack
 * contents differ (the PS4 version does not sink). Only bit 0 of the bool is used:
 *   and cl, 1; shl cl, 2; and byte [rax+0x80], 0xfb; or [rax+0x80], cl; nop  (same 20 bytes)
 * Builds 102 US and EU. BB_CONVOY_SINK=0: off. */
static void patch_u2_convoy_sink(uint64_t image_size) {
    const char *env=getenv("BB_CONVOY_SINK");
    if (env && env[0]=='0') return;
    /* (mov cl, [r+0x11];) mov dl, [rax+0x80]; shl cl, 2; and dl, 0xfb; or dl, cl; mov [rax+0x80], dl */
    const int64_t at=find_unique_pattern(image_size,"11 8a9080000000 c0e102 80e2fb 08ca 889080000000");
    if (at<0) return; /* not U2, or not unique */
    static const unsigned char code[20]={0x80,0xe1,0x01, 0xc0,0xe1,0x02, 0x80,0xa0,0x80,0x00,0x00,0x00,0xfb,
                                         0x08,0x88,0x80,0x00,0x00,0x00, 0x90};
    memcpy(image+at+1,code,sizeof(code));
    printf("Convoy sink: animation node flag uses only bit 0 of its bool (%#" PRIx64 ")\n",(uint64_t)at+1);
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
    const char *content_profile=NULL, *app0=NULL, *app0_update=NULL, *user_dir=NULL, *patch_file=NULL, *guest_argv_file=NULL, *trophy_dir=NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--list-missing")) list_missing = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--app0-update") && i+1<argc) app0_update=argv[++i]; /* issue #10 */
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
        runtime_file_configure_update(app0_update);
        if (app0_update) printf("Runtime: the update folder %s comes first under /app0\n", app0_update);
        runtime_file_configure(app0, user_dir ? user_dir : "user");
#ifdef _WIN32
        crash_dump_configure(user_dir ? user_dir : "user");
#endif
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
    if (getenv("BB_U1_VIEW_WATCH") && atoi(getenv("BB_U1_VIEW_WATCH"))>0) {
        pthread_t watch_thread;
        if (pthread_create(&watch_thread,NULL,u1_view_watch,NULL)==0) pthread_detach(watch_thread);
    }
    if (getenv("BB_U1_FRAME_WATCH") && atoi(getenv("BB_U1_FRAME_WATCH"))>0) {
        pthread_t frame_thread;
        if (pthread_create(&frame_thread,NULL,u1_frame_watch,NULL)==0) pthread_detach(frame_thread);
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
#ifdef _WIN32
    red_zone_reserve(image, round_page(size)); /* before bbgpu_init: the driver's allocations may fill its reach */
#endif
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
#ifndef _WIN32
        install_u1_view_lock(image_end);
        install_u1_set_job_guard(image_end);
        install_dt_smooth(image_end);
        install_u3_particle_job_guard(image_end);
#endif
        patch_job_lock_wait(image_end);
        patch_u1_sticky(image_end);
        runtime_display_patch(image, image_end);
        patch_u2_convoy_sink(image_end);
        patch_job_descriptor_guard(image_end);
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
