/* g2-arm64: guest_cpu.h on x86-64 hosts: guest code runs natively (the port's original model). */
#include "guest_cpu.h"
#if defined(__x86_64__) || defined(_M_X64)
#define ABI __attribute__((sysv_abi))
typedef uint64_t (ABI *Fn8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

int guest_cpu_init(void) { return 0; }
uintptr_t guest_host_function(const void *fn, const char *name, GuestAbi abi) {
    (void)name; (void)abi;
    return (uintptr_t)fn;
}
uint64_t guest_call(uintptr_t entry, const uint64_t *args, unsigned count) {
    uint64_t a[8] = {0};
    for (unsigned i = 0; i < count && i < 8; ++i) a[i] = args[i];
    /* Extra integer arguments are ignored by SysV callees (rdi..r9, then two stack slots). */
    return ((Fn8)entry)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}
#ifndef _WIN32
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
#endif
uint64_t guest_call_on_stack(uintptr_t entry, const uint64_t *args, unsigned count, void *stack_top) {
#ifdef _WIN32
    (void)stack_top;
    return guest_call(entry, args, count);
#else
    /* probe.c enter_on_stack: entry(arg0, arg1) on a new stack (the guest main thread). */
    enter_on_stack((void *)entry, (void *)(uintptr_t)(count > 0 ? args[0] : 0),
                   (void *)(uintptr_t)(count > 1 ? args[1] : 0), stack_top);
    return 0;
#endif
}
/* The TCB is installed by runtime_thread.c (arch_prctl / TEB slot) on x86-64. */
void guest_set_gs_base(uint64_t base) { (void)base; }
void guest_set_stack(void *stack_top) { (void)stack_top; }
int guest_cpu_signal(int sig, void *info, void *context) { (void)sig; (void)info; (void)context; return 0; }
int guest_context_get(GuestContext *out) { (void)out; return 0; }
int guest_context_set(const GuestContext *in) { (void)in; return 0; }
int guest_cpu_describe_thread(char *out, size_t size) { (void)out; (void)size; return 0; }
int guest_cpu_describe_stack(char *out, size_t size) { (void)out; (void)size; return 0; }
int guest_cpu_describe_registers(char *out, size_t size) { (void)out; (void)size; return 0; }
uint64_t guest_cpu_guest_rsp(void) { return 0; }
void guest_cpu_hle_trace_save(void) {}
#endif
