/*
 * AetherOS — USB Mass Storage Class: Bulk-Only Transport (Phase 5.2.12)
 * File: kernel/drivers/usb/msc.c
 *
 * Implements USB MSC Bulk-Only Transport (BOT) per the USB Mass Storage
 * Class Bulk-Only Transport specification 1.0.
 *
 * Protocol: CBW → (data) → CSW
 *   CBW: 31-byte Command Block Wrapper (OUT on bulk-OUT endpoint)
 *   Data: variable (IN for reads, OUT for writes)
 *   CSW: 13-byte Command Status Wrapper (IN on bulk-IN endpoint)
 *
 * SCSI commands implemented:
 *   TEST UNIT READY (0x00)
 *   INQUIRY (0x12)
 *   READ CAPACITY(10) (0x25)
 *   READ(10) (0x28)
 */

#include "drivers/usb/msc.h"
#include "drivers/usb/xhci.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── Freestanding helpers ────────────────────────────────────────────────── */

static void msc_zero(void *dst, u32 n)
{
    u8 *p = (u8 *)dst; while (n--) *p++ = 0;
}
static void msc_copy(void *dst, const void *src, u32 n)
{
    u8 *d = (u8 *)dst; const u8 *s = (const u8 *)src;
    while (n--) *d++ = *s++;
}

/* ── BOT Command Block Wrapper (CBW) — 31 bytes ──────────────────────────── */

#define CBW_SIGNATURE  0x43425355u   /* 'USBC' little-endian */
#define CSW_SIGNATURE  0x53425355u   /* 'USBS' little-endian */
#define CBW_FLAG_IN    0x80u         /* bmCBWFlags: direction = IN (device → host) */
#define CBW_FLAG_OUT   0x00u

typedef struct {
    u32 dCBWSignature;          /* 0x43425355 */
    u32 dCBWTag;
    u32 dCBWDataTransferLength;
    u8  bmCBWFlags;             /* bit 7: 1=IN (D2H), 0=OUT (H2D) */
    u8  bCBWLUN;                /* Logical Unit Number (0) */
    u8  bCBWCBLength;           /* Length of CBWCB (1-16) */
    u8  CBWCB[16];              /* SCSI Command Descriptor Block */
} __attribute__((packed)) usb_cbw_t;

typedef struct {
    u32 dCSWSignature;          /* 0x53425355 */
    u32 dCSWTag;
    u32 dCSWDataResidue;
    u8  bCSWStatus;             /* 0=good, 1=failed, 2=phase error */
} __attribute__((packed)) usb_csw_t;

/* ── MSC driver state ─────────────────────────────────────────────────────── */

static int  g_msc_ready;
static u8   g_slot_id;
static u8   g_ep_out;   /* 4-bit bulk OUT endpoint number */
static u8   g_ep_in;    /* 4-bit bulk IN endpoint number */
static u16  g_ep_mps;
static u32  g_tag;
static u32  g_sector_count;

/* xHCI DBI values for our bulk endpoints (assuming EP1 IN/OUT) */
#define MSC_DBI_OUT  2u   /* bulk OUT: DBI 2 (EP context index 1 → doorbell 2) */
#define MSC_DBI_IN   3u   /* bulk IN:  DBI 3 (EP context index 2 → doorbell 3) */

/* Static buffers — must be aligned for DMA */
static usb_cbw_t g_cbw  __attribute__((aligned(4)));
static usb_csw_t g_csw  __attribute__((aligned(4)));
static u8        g_inq_buf[36] __attribute__((aligned(4)));

/* ── BOT transfer helpers ────────────────────────────────────────────────── */

/*
 * bot_send_cbw — send a Command Block Wrapper on the bulk-OUT endpoint.
 * Returns 0 on success, -1 on error.
 */
static int bot_send_cbw(const usb_cbw_t *cbw)
{
    /* BOT CBW must be exactly 31 bytes; always direction OUT */
    int rc = xhci_bulk_xfer(g_slot_id, 1u, MSC_DBI_OUT,
                            (void *)cbw, 31u, 0 /* OUT */);
    if (rc != 31) {
        kwarn("MSC: CBW send failed (rc=%d)\n", rc);
        return -1;
    }
    return 0;
}

/*
 * bot_recv_csw — receive a Command Status Wrapper from bulk-IN endpoint.
 * Returns 0 on success, -1 on error.
 */
static int bot_recv_csw(void)
{
    msc_zero(&g_csw, sizeof(g_csw));
    int rc = xhci_bulk_xfer(g_slot_id, 2u, MSC_DBI_IN,
                            &g_csw, 13u, 1 /* IN */);
    if (rc < 13) {
        kwarn("MSC: CSW recv short (%d bytes)\n", rc);
        return -1;
    }
    if (g_csw.dCSWSignature != CSW_SIGNATURE) {
        kwarn("MSC: bad CSW signature 0x%x\n",
              (unsigned)g_csw.dCSWSignature);
        return -1;
    }
    if (g_csw.bCSWStatus != 0) {
        kwarn("MSC: CSW status=%u\n", (unsigned)g_csw.bCSWStatus);
        return -1;
    }
    return 0;
}

/*
 * bot_command — full BOT transaction: CBW + optional data + CSW.
 * cdb: SCSI command descriptor block
 * cdb_len: CDB length (6, 10, 12, or 16 bytes)
 * data_buf: data buffer (NULL if no data)
 * data_len: data length in bytes
 * flags: CBW_FLAG_IN or CBW_FLAG_OUT
 * Returns 0 on success, -1 on error.
 */
static int bot_command(const u8 *cdb, u8 cdb_len,
                       void *data_buf, u32 data_len, u8 flags)
{
    /* Build CBW */
    msc_zero(&g_cbw, sizeof(g_cbw));
    g_cbw.dCBWSignature          = CBW_SIGNATURE;
    g_cbw.dCBWTag                = ++g_tag;
    g_cbw.dCBWDataTransferLength = data_len;
    g_cbw.bmCBWFlags             = flags;
    g_cbw.bCBWLUN                = 0;
    g_cbw.bCBWCBLength           = cdb_len;
    msc_copy(g_cbw.CBWCB, cdb, cdb_len);

    /* Send CBW */
    if (bot_send_cbw(&g_cbw) < 0) return -1;

    /* Data phase */
    if (data_len && data_buf) {
        int dir_in = (flags & CBW_FLAG_IN) ? 1 : 0;
        int rc = xhci_bulk_xfer(g_slot_id,
                                dir_in ? 2u : 1u,
                                dir_in ? MSC_DBI_IN : MSC_DBI_OUT,
                                data_buf, data_len, dir_in);
        if (rc < 0) {
            kwarn("MSC: data phase failed\n");
            return -1;
        }
    }

    /* Receive CSW */
    return bot_recv_csw();
}

/* ── SCSI command helpers ────────────────────────────────────────────────── */

static int scsi_test_unit_ready(void)
{
    u8 cdb[6];
    msc_zero(cdb, sizeof(cdb));
    cdb[0] = 0x00;   /* TEST UNIT READY */
    return bot_command(cdb, 6, NULL, 0, CBW_FLAG_OUT);
}

static int scsi_inquiry(void)
{
    u8 cdb[6];
    msc_zero(cdb, sizeof(cdb));
    cdb[0] = 0x12;         /* INQUIRY */
    cdb[4] = sizeof(g_inq_buf);
    msc_zero(g_inq_buf, sizeof(g_inq_buf));
    return bot_command(cdb, 6, g_inq_buf, sizeof(g_inq_buf), CBW_FLAG_IN);
}

/*
 * scsi_read_capacity — READ CAPACITY(10).
 * out_sectors: last LBA + 1 (total sector count)
 * Returns 0 on success, -1 on error.
 */
static int scsi_read_capacity(u32 *out_sectors)
{
    u8 cdb[10];
    u8 buf[8];
    msc_zero(cdb, sizeof(cdb));
    msc_zero(buf, sizeof(buf));
    cdb[0] = 0x25;   /* READ CAPACITY(10) */

    if (bot_command(cdb, 10, buf, 8, CBW_FLAG_IN) < 0)
        return -1;

    /* Response: [0..3] = last LBA (big-endian), [4..7] = block size */
    u32 last_lba  = ((u32)buf[0] << 24) | ((u32)buf[1] << 16)
                  | ((u32)buf[2] << 8)  |  (u32)buf[3];
    u32 blk_size  = ((u32)buf[4] << 24) | ((u32)buf[5] << 16)
                  | ((u32)buf[6] << 8)  |  (u32)buf[7];

    kinfo("MSC: last_lba=%u blk_size=%u bytes\n",
          (unsigned)last_lba, (unsigned)blk_size);

    if (blk_size != 512u) {
        kwarn("MSC: non-512-byte sector size (%u) — unsupported\n",
              (unsigned)blk_size);
        return -1;
    }

    *out_sectors = last_lba + 1u;
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void usb_msc_probe(u8 slot_id, u8 ep_out, u8 ep_in, u16 ep_mps)
{
    g_slot_id = slot_id;
    g_ep_out  = ep_out;
    g_ep_in   = ep_in;
    g_ep_mps  = ep_mps;
    g_tag     = 0;

    kinfo("MSC: probing slot=%u ep_out=%u ep_in=%u mps=%u\n",
          (unsigned)slot_id, (unsigned)ep_out,
          (unsigned)ep_in, (unsigned)ep_mps);

    /* INQUIRY */
    if (scsi_inquiry() == 0) {
        /* vendor (bytes 8-15) + product (16-31) are ASCII */
        char vendor[9], product[17];
        for (int i = 0; i < 8;  i++) vendor[i]  = (char)(g_inq_buf[8  + i]);
        for (int i = 0; i < 16; i++) product[i] = (char)(g_inq_buf[16 + i]);
        vendor[8] = product[16] = '\0';
        kinfo("MSC: \"%s\" \"%s\"\n", vendor, product);
    }

    /* TEST UNIT READY (retry up to 3 times for drives that spin up slowly) */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (scsi_test_unit_ready() == 0) break;
        kinfo("MSC: unit not ready, retrying...\n");
    }

    /* READ CAPACITY */
    if (scsi_read_capacity(&g_sector_count) < 0) {
        kwarn("MSC: READ CAPACITY failed\n");
        return;
    }

    kinfo("MSC: disk ready — %u sectors (%u MB)\n",
          (unsigned)g_sector_count,
          (unsigned)(g_sector_count / 2048u));

    g_msc_ready = 1;
}

int usb_msc_ready(void) { return g_msc_ready; }

u32 usb_msc_sector_count(void) { return g_sector_count; }

int usb_msc_read_sectors(u64 lba, u32 count, u8 *buf)
{
    if (!g_msc_ready) return -1;
    if (!count)       return 0;

    /* READ(10): up to 65535 blocks per CDB */
    u32 remaining = count;
    u64 current_lba = lba;

    while (remaining > 0) {
        u32 n = remaining > 0xFFFFu ? 0xFFFFu : remaining;

        u8 cdb[10];
        msc_zero(cdb, sizeof(cdb));
        cdb[0] = 0x28;   /* READ(10) */
        cdb[2] = (u8)(current_lba >> 24);
        cdb[3] = (u8)(current_lba >> 16);
        cdb[4] = (u8)(current_lba >> 8);
        cdb[5] = (u8)(current_lba);
        cdb[7] = (u8)(n >> 8);
        cdb[8] = (u8)(n);

        u32 byte_count = n * 512u;
        if (bot_command(cdb, 10, buf, byte_count, CBW_FLAG_IN) < 0)
            return -1;

        buf         += byte_count;
        current_lba += n;
        remaining   -= n;
    }

    return 0;
}
