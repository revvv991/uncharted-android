/* g2-arm64: the boundary between guest (PS4 x86-64) code and the host runtime.
 * x86-64 hosts run guest code natively (guest_cpu_native.c): host functions are called
 * directly and guest pointers are called as functions. ARM64 hosts run it on FEXCore
 * (guest_cpu_fex.cpp): host functions get x86 veneers, guest calls go through the JIT.
 * Design: docs/design-guest-cpu.md in the uncharted-g2 repository. */
#ifndef BB_GUEST_CPU_H
#define BB_GUEST_CPU_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Calling convention the guest uses to call a host function. */
typedef enum { GUEST_ABI_SYSV = 0, GUEST_ABI_MS = 1 } GuestAbi;
/* Before any other call (FEX: context, veneer pages). 0 on success. */
int guest_cpu_init(void);
/* Guest-callable address of a host function (the function itself on x86-64). */
uintptr_t guest_host_function(const void *fn, const char *name, GuestAbi abi);
/* Calls guest code `entry` with up to 8 integer arguments (SysV) on the current thread and
 * returns RAX. A host thread without a guest context gets one on its current guest stack
 * (guest_set_stack) or a temporary one. */
uint64_t guest_call(uintptr_t entry, const uint64_t *args, unsigned count);
/* Runs guest code `entry` on the guest stack ending at `stack_top` (the guest main thread). */
uint64_t guest_call_on_stack(uintptr_t entry, const uint64_t *args, unsigned count, void *stack_top);
/* Guest TCB (the loader rewrites the eboot's fs:[0] loads to gs:[0]) of the current thread. */
void guest_set_gs_base(uint64_t base);
/* Guest stack of the current host thread for guest_call (guest threads: their PS4 stack). */
void guest_set_stack(void *stack_top);
/* First look at a fault signal (siginfo_t *, ucontext_t *): 1 when the backend handled it. */
int guest_cpu_signal(int sig, void *info, void *context);

/* Guest registers (x86 order: rax rcx rdx rbx rsp rbp rsi rdi r8..r15). Emulating backends only:
 * inside a host function the guest called, get returns the caller's state as it will resume
 * (rip in the veneer, before its ret) and set replaces it: the guest then resumes with the new
 * state and the host function's return value is ignored (set gpr[GUEST_RAX] instead). Both
 * return 0 when there is no such call (x86-64 hosts: always). Fibers use them on ARM64. */
enum { GUEST_RAX, GUEST_RCX, GUEST_RDX, GUEST_RBX, GUEST_RSP, GUEST_RBP, GUEST_RSI, GUEST_RDI,
       GUEST_R8, GUEST_R9, GUEST_R10, GUEST_R11, GUEST_R12, GUEST_R13, GUEST_R14, GUEST_R15 };
typedef struct {
    uint64_t gpr[16];
    uint64_t rip;
    uint32_t mxcsr;
    uint16_t fcw;
} GuestContext;
int guest_context_get(GuestContext *out);
int guest_context_set(const GuestContext *in);
/* Diagnostics (thread dumps): the current thread's guest state as text, e.g. the host function
 * it is in and the guest return address. Returns the length, 0 when there is nothing to say. */
int guest_cpu_describe_thread(char *out, size_t size);
/* g2-arm64: values on the current thread's guest stack that point into the low guest image (the
 * return addresses of the calls that led here, among others): a thread that waits in a guest loop
 * shows whose wait it is. */
int guest_cpu_describe_stack(char *out, size_t size);
/* g2-arm64: crash reports: all 16 guest registers (the state of the last host function call, or of
 * the last sync in guest code) and 256 bytes of guest memory at rbx; the guest rsp. */
int guest_cpu_describe_registers(char *out, size_t size);
uint64_t guest_cpu_guest_rsp(void);
/* g2-arm64: BB_HLE_TRACE_RING=1: writes the host function trace's last calls (the stall report
 * calls it once); nothing otherwise. */
void guest_cpu_hle_trace_save(void);
#define GUEST_CALL(entry, ...) \
    guest_call((uintptr_t)(entry), (const uint64_t[]){__VA_ARGS__}, \
               sizeof((const uint64_t[]){__VA_ARGS__}) / sizeof(uint64_t))
#define GUEST_CALL0(entry) guest_call((uintptr_t)(entry), NULL, 0)
#ifdef __cplusplus
}
#endif
#endif
