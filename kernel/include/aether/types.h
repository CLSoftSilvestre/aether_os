#ifndef AETHER_TYPES_H
#define AETHER_TYPES_H

/*
 * Fundamental types for AetherOS kernel.
 * We define our own rather than relying on stdint.h, since
 * the availability of hosted headers is not guaranteed in a
 * bare-metal toolchain.
 */

/* Exact-width integer types */
typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;

typedef signed char         s8;
typedef signed short        s16;
typedef signed int          s32;
typedef signed long long    s64;

/* Pointer-width integer (useful for address arithmetic) */
typedef unsigned long       uintptr_t;
typedef unsigned long       size_t;

/* Boolean */
typedef u32                 bool;
#define true  1
#define false 0

/* NULL */
#define NULL ((void *)0)

/* Volatile MMIO accessor — prevents compiler from optimising away I/O reads */
#define MMIO_READ32(addr)        (*(volatile u32 *)(addr))
#define MMIO_WRITE32(addr, val)  (*(volatile u32 *)(addr) = (val))

#endif /* AETHER_TYPES_H */
