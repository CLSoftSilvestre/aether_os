#ifndef DRIVERS_POWER_BOOT_PROF_H
#define DRIVERS_POWER_BOOT_PROF_H
/*
 * AetherOS — Boot Time Profiler (Phase 6.2.5)
 * File: kernel/include/drivers/power/boot_prof.h
 *
 * Records CNTPCT_EL0 timestamps at named points during kernel_main() and
 * prints a table showing elapsed time (µs) between consecutive stamps once
 * boot is complete.
 *
 * Usage:
 *   boot_prof_stamp("uart");     // call immediately after uart_init()
 *   boot_prof_stamp("pmm");      // ...
 *   boot_prof_print();           // call just before enabling IRQs
 *
 * Target: < 2 s to first pixel on desktop (6.2.5 goal).
 */

#define BOOT_PROF_MAX_STAMPS  20

void boot_prof_stamp(const char *label);   /* record current CNTPCT_EL0 */
void boot_prof_print(void);                /* print elapsed-time table   */

#endif /* DRIVERS_POWER_BOOT_PROF_H */
