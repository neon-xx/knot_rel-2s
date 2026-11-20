#include "knot_midi_queue.h"
#include <stdint.h>

#include "freertos/FreeRTOS.h"

/*
 * Single-slot-empty ring buffers shared between tasks (UART RX, USB client
 * callbacks, TX tasks). Every access is done inside a critical section so
 * concurrent producers/consumers cannot corrupt the indices, and a push into
 * a full queue is refused (return 1) instead of silently wrapping over the
 * unread events.
 */

#define KNOT_MIDI_QUEUE_USBOUT_LENGTH 128
static uint32_t knot_midi_queueu_usbout_write_index = 0;
static uint32_t knot_midi_queueu_usbout_read_index = 0;
static struct usb_midi_event_packet knot_midi_queue_usbout[KNOT_MIDI_QUEUE_USBOUT_LENGTH] = {0};
static portMUX_TYPE knot_midi_queue_usbout_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t knot_midi_queue_usbout_dropped = 0;

int knot_midi_queue_usbout_push(struct usb_midi_event_packet ev) {

  if (ev.byte0 == 0 && ev.byte1 == 0 && ev.byte2 == 0 && ev.byte3 == 0) {
    return 0; // nothing to send
  }

  int status = 0;
  portENTER_CRITICAL_SAFE(&knot_midi_queue_usbout_mux);
  uint32_t next = (knot_midi_queueu_usbout_write_index + 1) % KNOT_MIDI_QUEUE_USBOUT_LENGTH;
  if (next == knot_midi_queueu_usbout_read_index) {
    knot_midi_queue_usbout_dropped++;
    status = 1; // full
  } else {
    knot_midi_queue_usbout[knot_midi_queueu_usbout_write_index] = ev;
    knot_midi_queueu_usbout_write_index = next;
  }
  portEXIT_CRITICAL_SAFE(&knot_midi_queue_usbout_mux);
  return status;
}

int knot_midi_queue_usbout_available(void) {
  portENTER_CRITICAL_SAFE(&knot_midi_queue_usbout_mux);
  int available = (knot_midi_queueu_usbout_read_index != knot_midi_queueu_usbout_write_index);
  portEXIT_CRITICAL_SAFE(&knot_midi_queue_usbout_mux);
  return available;
}

int knot_midi_queue_usbout_pop(struct usb_midi_event_packet* ev) {

  int status = 1;
  portENTER_CRITICAL_SAFE(&knot_midi_queue_usbout_mux);
  if (knot_midi_queueu_usbout_read_index != knot_midi_queueu_usbout_write_index) {
    *ev = knot_midi_queue_usbout[knot_midi_queueu_usbout_read_index];
    knot_midi_queueu_usbout_read_index = (knot_midi_queueu_usbout_read_index + 1) % KNOT_MIDI_QUEUE_USBOUT_LENGTH;
    status = 0;
  }
  portEXIT_CRITICAL_SAFE(&knot_midi_queue_usbout_mux);
  return status;
}

#define KNOT_MIDI_QUEUE_TRSOUT_LENGTH 128
static uint32_t knot_midi_queueu_trsout_write_index = 0;
static uint32_t knot_midi_queueu_trsout_read_index = 0;
static struct uart_midi_event_packet knot_midi_queue_trsout[KNOT_MIDI_QUEUE_TRSOUT_LENGTH] = {0};
static portMUX_TYPE knot_midi_queue_trsout_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t knot_midi_queue_trsout_dropped = 0;

int knot_midi_queue_trsout_push(struct uart_midi_event_packet ev) {

  if (ev.length == 0) {
    return 0; // nothing to send
  }

  int status = 0;
  portENTER_CRITICAL_SAFE(&knot_midi_queue_trsout_mux);
  uint32_t next = (knot_midi_queueu_trsout_write_index + 1) % KNOT_MIDI_QUEUE_TRSOUT_LENGTH;
  if (next == knot_midi_queueu_trsout_read_index) {
    knot_midi_queue_trsout_dropped++;
    status = 1; // full
  } else {
    knot_midi_queue_trsout[knot_midi_queueu_trsout_write_index] = ev;
    knot_midi_queueu_trsout_write_index = next;
  }
  portEXIT_CRITICAL_SAFE(&knot_midi_queue_trsout_mux);
  return status;
}

int knot_midi_queue_trsout_available(void) {
  portENTER_CRITICAL_SAFE(&knot_midi_queue_trsout_mux);
  int available = (knot_midi_queueu_trsout_read_index != knot_midi_queueu_trsout_write_index);
  portEXIT_CRITICAL_SAFE(&knot_midi_queue_trsout_mux);
  return available;
}

int knot_midi_queue_trsout_pop(struct uart_midi_event_packet* ev) {

  int status = 1;
  portENTER_CRITICAL_SAFE(&knot_midi_queue_trsout_mux);
  if (knot_midi_queueu_trsout_read_index != knot_midi_queueu_trsout_write_index) {
    *ev = knot_midi_queue_trsout[knot_midi_queueu_trsout_read_index];
    knot_midi_queueu_trsout_read_index = (knot_midi_queueu_trsout_read_index + 1) % KNOT_MIDI_QUEUE_TRSOUT_LENGTH;
    status = 0;
  }
  portEXIT_CRITICAL_SAFE(&knot_midi_queue_trsout_mux);
  return status;
}
