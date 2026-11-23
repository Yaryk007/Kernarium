// SPDX-License-Identifier: GPL-2.0-or-later
#include <signal.h>
enum um_irq_type { IRQ_READ, IRQ_WRITE };
struct siginfo;
struct uml_pt_regs;
void sigio_handler(int sig, struct siginfo *unused_si, struct uml_pt_regs *regs, void *mc);
