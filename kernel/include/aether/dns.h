/*
 * AetherOS — DNS stub resolver
 * File: kernel/include/aether/dns.h
 */
#ifndef AETHER_DNS_H
#define AETHER_DNS_H

#include "aether/types.h"

/* Resolve a hostname to its first A-record IP (host byte order).
 * Returns 0 on failure or timeout (1 s). */
u32 dns_resolve(const char *hostname);

#endif /* AETHER_DNS_H */
