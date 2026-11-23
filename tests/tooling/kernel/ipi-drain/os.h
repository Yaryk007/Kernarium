// SPDX-License-Identifier: GPL-2.0-or-later
// The kernel interfaces os-Darwin/smp.c and time.c use, as the test needs them.
#ifndef FX_OS_H
#define FX_OS_H
#include <stdarg.h>
#include <setjmp.h>
#define CONFIG_NR_CPUS 4
#define CONFIG_SMP 1
#define IS_ENABLED(option) (option)
#define CONFIG_TRACE_IRQFLAGS 0
#define UM_NSEC_PER_SEC 1000000000ULL
extern int uml_ncpus;
void panic(const char *, ...) __attribute__((noreturn));
void os_warn(const char *, ...);
int um_set_signals(int enable);
int um_get_signals(void);
void um_trace_signals_on(void);
void um_trace_signals_off(void);
void unblock_signals(void);
#define noinline __attribute__((noinline))
#define UML_LONGJMP(buffer, value) longjmp(*(buffer), (value))
void uml_ipi_handler_mask(unsigned int pending);
void darwin_timer_kernel_checkpoint(void);
void set_sigstack(void *stack, int size);
void new_thread(void *stack, jmp_buf *buf, void (*handler)(void));
void new_thread_handler(void);
void uml_finishsetup(void);
void uml_start_secondary(void *argument);
int os_set_fd_block(int fd, int blocking);
int os_set_exec_close(int fd);
struct uml_pt_regs { long regs[4]; };
int uml_curr_cpu(void);
int darwin_timers_cooperative(void);
void timer_handler(int sig, void *unused_si, struct uml_pt_regs *regs);
int darwin_uml_stop_fd(void);
#endif
