/*
 * AetherOS — USB MIDI 1.0 driver header (Phase 8.1)
 * File: kernel/include/drivers/usb/midi/usb_midi.h
 */

#ifndef AETHER_USB_MIDI_H
#define AETHER_USB_MIDI_H

#include "aether/types.h"

/* MIDI event packed for syscall ring */
typedef struct {
    u8 status;   /* status byte: 0x80-0xFF */
    u8 data1;
    u8 data2;
    u8 _pad;
} midi_event_t;

/* Max events in kernel ring buffer */
#define MIDI_RING_SIZE 256

void usb_midi_init(void);
void usb_midi_poll(void);

/* Called from syscall.c SYS_MIDI_READ */
int  usb_midi_read(midi_event_t *out, int max_events);

/* Called from syscall.c SYS_MIDI_WRITE */
int  usb_midi_write(const midi_event_t *events, int count);

#endif /* AETHER_USB_MIDI_H */
