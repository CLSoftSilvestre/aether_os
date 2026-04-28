/*
 * AetherOS — DNS stub resolver (Phase 5.1)
 * File: kernel/net/dns.c
 *
 * Sends a single A-record UDP query to g_dns_ip:53.
 * Returns the first A-record IP, or 0 on failure.
 * QEMU user networking: DNS server at 10.0.2.3.
 */

#include "aether/dns.h"
#include "aether/net.h"
#include "aether/udp.h"
#include "aether/printk.h"
#include "aether/types.h"

#define DNS_PORT    53u
#define DNS_SRC_PORT 5353u

/* ── DNS header ──────────────────────────────────────────────────────────── */
typedef struct {
    u16 id;
    u16 flags;
    u16 qdcount;
    u16 ancount;
    u16 nscount;
    u16 arcount;
} __attribute__((packed)) dns_hdr_t;

#define DNS_HDR_LEN  12u
#define DNS_BUF_MAX  512u

/* ── State ───────────────────────────────────────────────────────────────── */
static volatile u32 g_dns_result;
static volatile int g_dns_done;
static u16          g_dns_query_id;

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static void nm_copy(void *d, const void *s, u32 n) {
    u8 *dp=(u8*)d; const u8 *sp=(const u8*)s; while(n--) *dp++=*sp++;
}
static int nm_strlen(const char *s) { int n=0; while(s[n]) n++; return n; }

/* ── Build DNS A-query ───────────────────────────────────────────────────── */
static int dns_build_query(u8 *buf, const char *hostname, u16 id)
{
    dns_hdr_t *hdr = (dns_hdr_t *)buf;
    hdr->id      = net_htons(id);
    hdr->flags   = net_htons(0x0100u);  /* RD=1 */
    hdr->qdcount = net_htons(1u);
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;

    int pos = DNS_HDR_LEN;

    /* Encode hostname as DNS labels (e.g. "foo.bar" → \x03foo\x03bar\x00) */
    const char *p = hostname;
    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int label_len = (int)(dot - p);
        if (label_len == 0 || label_len > 63) return -1;
        buf[pos++] = (u8)label_len;
        nm_copy(buf + pos, p, (u32)label_len);
        pos += label_len;
        if (*dot == '.') dot++;
        p = dot;
    }
    buf[pos++] = 0;      /* root label */
    buf[pos++] = 0;      /* QTYPE A hi */
    buf[pos++] = 1;      /* QTYPE A lo */
    buf[pos++] = 0;      /* QCLASS IN hi */
    buf[pos++] = 1;      /* QCLASS IN lo */

    return pos;
}

/* ── DNS response parser ─────────────────────────────────────────────────── */

/* Skip a DNS name starting at buf[off]; returns new offset */
static int dns_skip_name(const u8 *buf, int off, int len)
{
    while (off < len) {
        u8 c = buf[off];
        if (c == 0) return off + 1;
        if ((c & 0xC0u) == 0xC0u) return off + 2;  /* pointer */
        off += 1 + (int)(c & 0x3Fu);
    }
    return off;
}

static u32 dns_parse_response(const u8 *buf, int len, u16 expected_id)
{
    if (len < DNS_HDR_LEN) return 0;
    const dns_hdr_t *hdr = (const dns_hdr_t *)buf;
    if (net_ntohs(hdr->id) != expected_id) return 0;
    if (!(net_ntohs(hdr->flags) & 0x8000u)) return 0;  /* not a response */

    int qdcount = (int)net_ntohs(hdr->qdcount);
    int ancount = (int)net_ntohs(hdr->ancount);
    int off = DNS_HDR_LEN;

    /* Skip questions */
    for (int i = 0; i < qdcount; i++) {
        off = dns_skip_name(buf, off, len);
        off += 4;  /* QTYPE + QCLASS */
    }

    /* Parse answers */
    for (int i = 0; i < ancount; i++) {
        off = dns_skip_name(buf, off, len);
        if (off + 10 > len) break;
        u16 rtype  = ((u16)buf[off] << 8) | buf[off+1]; off += 2;
        off += 2;  /* class */
        off += 4;  /* TTL */
        u16 rdlen  = ((u16)buf[off] << 8) | buf[off+1]; off += 2;

        if (rtype == 1u && rdlen == 4u && off + 4 <= len) {
            /* A record */
            return ((u32)buf[off]   << 24) | ((u32)buf[off+1] << 16) |
                   ((u32)buf[off+2] <<  8) |  (u32)buf[off+3];
        }
        off += (int)rdlen;
    }
    return 0;
}

/* ── UDP handler for DNS responses ──────────────────────────────────────── */
static void dns_udp_handler(u32 src_ip, u16 src_port,
                             const u8 *data, u16 data_len)
{
    (void)src_ip; (void)src_port;
    if (g_dns_done) return;
    u32 ip = dns_parse_response(data, (int)data_len, g_dns_query_id);
    if (ip) { g_dns_result = ip; g_dns_done = 1; }
}

/* ── Public: dns_resolve ─────────────────────────────────────────────────── */

u32 dns_resolve(const char *hostname)
{
    if (!g_dns_ip) { kwarn("dns_resolve: no DNS server\n"); return 0; }

    extern u64 timer_get_freq(void);
    extern void virtio_net_rx_poll(void);
    u64 freq = timer_get_freq();
    if (!freq) freq = 62500000ULL;

    /* Derive query ID from counter */
    u64 t; __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(t));
    g_dns_query_id = (u16)(t ^ (t >> 16));
    g_dns_result   = 0;
    g_dns_done     = 0;

    u8 qbuf[DNS_BUF_MAX];
    int qlen = dns_build_query(qbuf, hostname, g_dns_query_id);
    if (qlen < 0) { kwarn("dns_resolve: bad hostname\n"); return 0; }

    udp_bind(DNS_SRC_PORT, dns_udp_handler);

    for (int attempt = 0; attempt < 2 && !g_dns_done; attempt++) {
        udp_send(g_dns_ip, DNS_PORT, DNS_SRC_PORT, qbuf, (u16)qlen);

        u64 start; __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(start));
        while (!g_dns_done) {
            virtio_net_rx_poll();
            u64 now; __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(now));
            if ((now - start) >= freq) break;
        }
    }

    udp_unbind(DNS_SRC_PORT);

    if (g_dns_done) {
        kinfo("DNS: %s → %u.%u.%u.%u\n", hostname,
              (g_dns_result >> 24) & 0xFF, (g_dns_result >> 16) & 0xFF,
              (g_dns_result >>  8) & 0xFF,  g_dns_result         & 0xFF);
    } else {
        kwarn("DNS: timeout resolving '%s'\n", hostname);
    }

    return g_dns_result;
}
