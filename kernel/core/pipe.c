/*
 * AetherOS — Kernel Pipe
 * File: kernel/core/pipe.c
 *
 * Simple ring-buffer pipe.  Both ends are reference-counted so the kernel
 * knows when each side has been closed.  Reads block (via task_yield) when
 * the buffer is empty and the write end is still open.
 */

#include "aether/pipe.h"
#include "aether/scheduler.h"
#include "aether/printk.h"
#include "aether/types.h"

typedef struct {
    u8  buf[PIPE_BUF];
    u32 head;         /* next write position */
    u32 tail;         /* next read  position */
    u32 read_open;    /* number of tasks holding the read end  */
    u32 write_open;   /* number of tasks holding the write end */
} pipe_t;

static pipe_t g_pipes[MAX_PIPES];

void pipe_init(void)
{
    for (int i = 0; i < MAX_PIPES; i++) {
        g_pipes[i].head       = 0;
        g_pipes[i].tail       = 0;
        g_pipes[i].read_open  = 0;
        g_pipes[i].write_open = 0;
    }
}

int pipe_alloc(void)
{
    for (int i = 0; i < MAX_PIPES; i++) {
        pipe_t *p = &g_pipes[i];
        if (p->read_open == 0 && p->write_open == 0) {
            p->head       = 0;
            p->tail       = 0;
            p->read_open  = 1;
            p->write_open = 1;
            kinfo("Pipe: allocated slot %d\n", i);
            return i;
        }
    }
    kerror("Pipe: no free slots\n");
    return -1;
}

void pipe_close_read(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES) return;
    pipe_t *p = &g_pipes[idx];
    if (p->read_open > 0) p->read_open--;
}

void pipe_close_write(int idx)
{
    if (idx < 0 || idx >= MAX_PIPES) return;
    pipe_t *p = &g_pipes[idx];
    if (p->write_open > 0) p->write_open--;
}

static u32 pipe_used(const pipe_t *p)
{
    return (p->head - p->tail) & (PIPE_BUF - 1);
}

static u32 pipe_free_space(const pipe_t *p)
{
    return (PIPE_BUF - 1) - pipe_used(p);
}

long pipe_read(int idx, char *buf, long len)
{
    if (idx < 0 || idx >= MAX_PIPES || len <= 0) return -1;
    pipe_t *p = &g_pipes[idx];

    /* Block while empty, but stop if write end closed */
    while (p->head == p->tail) {
        if (!p->write_open) return 0;  /* EOF */
        task_yield();
    }

    long n = 0;
    while (n < len && p->head != p->tail) {
        buf[n++] = (char)p->buf[p->tail & (PIPE_BUF - 1)];
        p->tail++;
    }
    return n;
}

long pipe_write(int idx, const char *buf, long len)
{
    if (idx < 0 || idx >= MAX_PIPES || len <= 0) return -1;
    pipe_t *p = &g_pipes[idx];

    if (!p->read_open) return -1;  /* broken pipe */

    long n = 0;
    while (n < len) {
        while (pipe_free_space(p) == 0) {
            if (!p->read_open) return n ? n : -1;
            task_yield();
        }
        p->buf[p->head & (PIPE_BUF - 1)] = (u8)buf[n++];
        p->head++;
    }
    return n;
}
