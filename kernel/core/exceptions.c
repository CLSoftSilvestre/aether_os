/*
 * AetherOS — Exception Handlers (C side)
 * File: kernel/core/exceptions.c
 *
 * These functions are called from exceptions.S after the CPU context
 * is saved into a trap_frame_t on the kernel stack.
 *
 * For Phase 1, synchronous faults and SErrors trigger a kernel panic
 * with a register dump. IRQs are dispatched to the GIC and then to
 * the appropriate driver handler (timer, UART, etc.).
 */

#include "aether/exceptions.h"
#include "aether/printk.h"
#include "aether/syscall.h"
#include "drivers/irq/gic_v2.h"
#include "drivers/timer/arm_timer.h"
#include "drivers/char/uart_pl011.h"

/* ── Vector table installation ──────────────────────────────────────────── */

/* Symbol defined in exceptions.S */
extern void vector_table(void);

/*
 * exceptions_init — load the vector table address into VBAR_EL1.
 *
 * Until this is called, any exception (including IRQs) will jump to
 * address 0x0 — which is either the QEMU boot stub or garbage — and hang.
 */
void exceptions_init(void)
{
    __asm__ volatile(
        "msr VBAR_EL1, %0\n"
        "isb\n"                 /* isb ensures VBAR takes effect immediately */
        :: "r"((uintptr_t)vector_table)
        : "memory"
    );

    kinfo("Exceptions: vector table installed at %p\n", (void *)vector_table);
}

/* ── Fault information printer ──────────────────────────────────────────── */

/*
 * Decode and print the Exception Syndrome Register.
 *
 * ESR_EL1 tells us exactly WHY the exception was taken — fault type,
 * faulting instruction length, and additional details.
 *
 * In x86 terms: this is like reading the error code pushed onto the stack
 * by the CPU during a page fault or GPF, but far more detailed.
 */
static void print_exception_info(const trap_frame_t *frame, const char *type)
{
    u32 ec  = ESR_EC(frame->esr);
    u32 iss = ESR_ISS(frame->esr);

    const char *ec_name;
    switch (ec) {
    case EC_UNKNOWN:    ec_name = "Unknown";                    break;
    case EC_WFI:        ec_name = "WFI/WFE trapped";           break;
    case EC_SVC64:      ec_name = "SVC (syscall)";             break;
    case EC_IABORT_LOW: ec_name = "Instruction Abort (lower EL)"; break;
    case EC_IABORT_CUR: ec_name = "Instruction Abort (current EL)"; break;
    case EC_PC_ALIGN:   ec_name = "PC Alignment Fault";        break;
    case EC_DABORT_LOW: ec_name = "Data Abort (lower EL)";     break;
    case EC_DABORT_CUR: ec_name = "Data Abort (current EL)";   break;
    case EC_SP_ALIGN:   ec_name = "SP Alignment Fault";        break;
    case EC_BRK:        ec_name = "BRK (software breakpoint)"; break;
    default:            ec_name = "Unrecognised EC";           break;
    }

    kerror("═══════════════════════════════════════════\n");
    kerror("EXCEPTION: %s\n", type);
    kerror("  ELR  (PC):  %p\n",   (void *)frame->elr);
    kerror("  SPSR:       0x%lx\n", (unsigned long)frame->spsr);
    kerror("  ESR:        0x%lx\n", (unsigned long)frame->esr);
    kerror("  EC:  0x%x  — %s\n",  ec, ec_name);
    kerror("  ISS: 0x%x\n",        iss);
    kerror("─── Registers ─────────────────────────────\n");

    for (int i = 0; i < 30; i += 2) {
        kerror("  x%-2d: %p    x%-2d: %p\n",
               i,   (void *)frame->x[i],
               i+1, (void *)frame->x[i+1]);
    }
    kerror("  x30: %p\n", (void *)frame->x30);
    kerror("═══════════════════════════════════════════\n");
}

/* ── Handlers called from exceptions.S ─────────────────────────────────── */

/*
 * el1_sync_handler — kernel synchronous exception.
 *
 * Triggered by: data aborts, instruction aborts, alignment faults,
 *               undefined instructions, BRK, SVC from kernel.
 *
 * In Phase 1 all of these are fatal (kernel panic).
 * In Phase 3, SVC from EL0 will be handled by el0_sync_handler instead.
 */
void el1_sync_handler(trap_frame_t *frame)
{
    print_exception_info(frame, "Synchronous (EL1)");
    kpanic("Unhandled kernel exception — halting\n");
}

/*
 * el1_irq_handler — hardware interrupt (IRQ) from kernel context.
 *
 * 1. Acknowledge the GIC (get IRQ ID)
 * 2. Dispatch to the driver that owns that IRQ
 * 3. Signal End Of Interrupt to the GIC
 *
 * Currently only IRQ 30 (ARM Generic Timer) is registered.
 */
void el1_irq_handler(trap_frame_t *frame)
{
    (void)frame;   /* not needed for IRQ dispatch — suppress warning */

    /* Step 1: claim the IRQ */
    u32 irq = gic_acknowledge();

    if (irq == GIC_SPURIOUS_IRQ)
        return;   /* spurious — no action needed */

    /* Step 2: dispatch */
    switch (irq) {
    case TIMER_IRQ_ID:
        timer_irq_handler();
        break;

    case UART_IRQ_ID:
        uart_irq_handler();
        break;

    default:
        kwarn("IRQ: unhandled interrupt ID %lu\n", (unsigned long)irq);
        break;
    }

    /* Step 3: end of interrupt */
    gic_end_of_interrupt(irq);
}

/*
 * el1_serror_handler — System Error (async abort).
 *
 * SErrors are typically caused by uncorrectable memory errors or bus faults.
 * Always fatal.
 */
void el1_serror_handler(trap_frame_t *frame)
{
    print_exception_info(frame, "SError (System Error)");
    kpanic("SError — unrecoverable hardware fault\n");
}

/*
 * el0_sync_handler — synchronous exception from user space.
 *
 * Phase 3 implementation: SVC #0 will dispatch syscalls here.
 * For now, any user-space fault panics the kernel.
 */
void el0_sync_handler(trap_frame_t *frame)
{
    if (ESR_EC(frame->esr) == EC_SVC64) {
        frame->x[0] = (u64)syscall_dispatch(frame);
        return;
    }
    print_exception_info(frame, "Synchronous (EL0 — user)");
    kpanic("Unhandled user exception — halting\n");
}

/*
 * el0_irq_handler — IRQ that arrived while CPU was in user space (EL0).
 *
 * The CPU automatically switches to EL1 / SP_EL1 before calling this.
 * Behaviour is identical to el1_irq_handler for now.
 */
void el0_irq_handler(trap_frame_t *frame)
{
    el1_irq_handler(frame);
}
