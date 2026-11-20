#include "knot_midi_translator.h"
#include <stdint.h>

struct uart_midi_event_packet usb_midi_to_uart(struct usb_midi_event_packet usb_packet) {

  struct uart_midi_event_packet uart_packet = {.length = 0, .byte1 = usb_packet.byte1, .byte2 = usb_packet.byte2, .byte3 = usb_packet.byte3};

  /*

  32-bit USB-MIDI Event Packet

  | ==== Byte 0 ==== | ==== Byte 1 ==== | ==== Byte 2 ==== | ==== Byte 3 ==== |
  | Cable  | Code    |                  |                  |                  |
  | Number | Index   |      MIDI_0      |      MIDI_1      |      MIDI_2      |
  |        | Number  |                  |                  |                  |


  */

  // MIDI Cable Number
  uint8_t cn = ((usb_packet.byte0 & 0b11110000) >> 4);

  // MIDI Code Index Number
  uint8_t cin = (usb_packet.byte0 & 0b00001111);

  switch (cin) {
  case 0x00:
    break; // Miscellaneous function codes. Reserved for future extensions.
  case 0x01:
    break; // Cable events. Reserved for future expansion.
  case 0x02:
    uart_packet.length = 2;
    break; // Two-byte System Common messages like MTC, SongSelect, etc.
  case 0x03:
    uart_packet.length = 3;
    break; // Three-byte System Common messages like SPP, etc.
  case 0x04:
    uart_packet.length = 3;
    break; // SysEx starts or continues
  case 0x05:
    uart_packet.length = 1;
    break; // Single-byte System Common Message or SysEx ends with following single byte.
  case 0x06:
    uart_packet.length = 2;
    break; // SysEx ends with following two bytes.
  case 0x07:
    uart_packet.length = 3;
    break; // SysEx ends with following three bytes.
  case 0x08:
    uart_packet.length = 3;
    break; // Note-off
  case 0x09:
    uart_packet.length = 3;
    break; // Note-on
  case 0x0A:
    uart_packet.length = 3;
    break; // Poly-KeyPress
  case 0x0B:
    uart_packet.length = 3;
    break; // Control Change
  case 0x0C:
    uart_packet.length = 2;
    break; // Program Change
  case 0x0D:
    uart_packet.length = 2;
    break; // Channel Pressure
  case 0x0E:
    uart_packet.length = 3;
    break; // PitchBend Change
  case 0x0F:
    uart_packet.length = 1;
    break; // Single Byte
  }

  return uart_packet;
}

uint8_t uart_midi_is_byte_rtm(uint8_t byte) {

  if (byte >= 0xF8) {
    return 1;
  } else {
    return 0;
  }
}

uint8_t uart_midi_processor_buffer[3] = {0};
uint8_t uart_midi_processor_buffer_index = 0;
uint8_t uart_midi_processor_buffer_limit = 0;
uint8_t uart_midi_processor_state_is_sysex = 0;

struct usb_midi_event_packet midi_uart_to_usb(struct uart_midi_event_packet uart_packet) {

  // initialize return packet
  struct usb_midi_event_packet ev = {.byte0 = 0, .byte1 = uart_packet.byte1, .byte2 = uart_packet.byte2, .byte3 = uart_packet.byte3};

  if (uart_packet.length == 0) {
    ev.byte1 = 0;
    ev.byte2 = 0;
    ev.byte3 = 0;
    return ev;
  }

  if (uart_packet.byte1 == 0xF0 || uart_packet.byte1 < 128) {
    // Sysex start or continue
    if (uart_packet.byte2 == 0xF7) {
      ev.byte0 = 0x06; // SysEx ends with following two bytes
    } else if (uart_packet.byte3 == 0xF7) {
      ev.byte0 = 0x07; // SysEx ends with following three bytes
    } else {
      ev.byte0 = 0x04; // SysEx starts or continues
    }
    return ev;
  }

  switch (uart_packet.byte1) {
  case 0xF7: // Sysex stop (single byte)
  case 0xF6: // Tune Request
    ev.byte0 = 0x05; // Single-byte System Common message or SysEx end
    break;
  case 0xF1: // MTC Quarter Frame
  case 0xF3: // Song Select
    ev.byte0 = 0x02; // Two-byte System Common message
    break;
  case 0xF2: // Song Position Pointer
    ev.byte0 = 0x03; // Three-byte System Common message
    break;
  default:
    // Channel Voice Message (CIN = status high nibble) or Real-Time (CIN 0xF)
    ev.byte0 = (uart_packet.byte1 >> 4);
    break;
  }

  return ev;
}

static void uart_midi_processor_clear(void) {
  uart_midi_processor_buffer[0] = 0;
  uart_midi_processor_buffer[1] = 0;
  uart_midi_processor_buffer[2] = 0;
  uart_midi_processor_buffer_index = 0;
  uart_midi_processor_buffer_limit = 0; // no running status
}

void uart_midi_processor_reset(void) {
  uart_midi_processor_clear();
  uart_midi_processor_state_is_sysex = 0;
}

static struct uart_midi_event_packet uart_midi_processor_assemble(void) {
  return (struct uart_midi_event_packet){
      .length = uart_midi_processor_buffer_index, .byte1 = uart_midi_processor_buffer[0], .byte2 = uart_midi_processor_buffer[1], .byte3 = uart_midi_processor_buffer[2]};
}

struct uart_midi_event_packet uart_midi_process_byte(uint8_t byte) {

  // initialize return packet
  struct uart_midi_event_packet ev = {.length = 0, .byte1 = 0, .byte2 = 0, .byte3 = 0};

  // Real-Time Messages are single bytes that may appear anywhere (even inside
  // SysEx or between the bytes of another message) and do not affect any state.
  if (uart_midi_is_byte_rtm(byte)) {
    return (struct uart_midi_event_packet){.length = 1, .byte1 = byte, .byte2 = 0, .byte3 = 0};
  }

  if (uart_midi_processor_state_is_sysex) {

    if (byte == 0xF7) {

      // store sysex end and emit what is buffered
      uart_midi_processor_buffer[uart_midi_processor_buffer_index++] = byte;
      ev = uart_midi_processor_assemble();

      // switch to normal mode, SysEx cancels Running Status
      uart_midi_processor_state_is_sysex = 0;
      uart_midi_processor_clear();
      return ev;
    }

    if (byte < 0x80) {

      // store sysex data
      uart_midi_processor_buffer[uart_midi_processor_buffer_index++] = byte;

      if (uart_midi_processor_buffer_index == 3) {
        // no more space, send packet and continue in sysex mode
        ev = uart_midi_processor_assemble();
        uart_midi_processor_clear();
      }
      return ev;
    }

    // Any other status byte terminates an unfinished SysEx. The partial chunk
    // is dropped and the status byte is processed normally below.
    uart_midi_processor_state_is_sysex = 0;
    uart_midi_processor_clear();
  }

  if (byte == 0xF0) {
    // switch to sysex mode, store sysex start command
    uart_midi_processor_state_is_sysex = 1;
    uart_midi_processor_clear();
    uart_midi_processor_buffer[0] = byte;
    uart_midi_processor_buffer_index = 1;
    return ev;
  }

  if (byte >= 0x80) {

    uart_midi_processor_clear();

    if (byte < 0xF0) {
      // Channel Voice Message: becomes the Running Status
      uart_midi_processor_buffer[0] = byte;
      uart_midi_processor_buffer_index = 1;

      if ((byte & 0xF0) == 0xC0 || (byte & 0xF0) == 0xD0) {
        // program change and channel pressure have one data byte
        uart_midi_processor_buffer_limit = 2;
      } else {
        uart_midi_processor_buffer_limit = 3;
      }
      return ev;
    }

    // System Common Messages (cancel Running Status)
    switch (byte) {
    case 0xF1: // MTC Quarter Frame
    case 0xF3: // Song Select
      uart_midi_processor_buffer[0] = byte;
      uart_midi_processor_buffer_index = 1;
      uart_midi_processor_buffer_limit = 2;
      break;
    case 0xF2: // Song Position Pointer
      uart_midi_processor_buffer[0] = byte;
      uart_midi_processor_buffer_index = 1;
      uart_midi_processor_buffer_limit = 3;
      break;
    case 0xF6: // Tune Request, no data bytes
      ev = (struct uart_midi_event_packet){.length = 1, .byte1 = byte, .byte2 = 0, .byte3 = 0};
      break;
    default:
      // 0xF4, 0xF5 (undefined) and stray 0xF7 (EOX outside SysEx): ignore
      break;
    }
    return ev;
  }

  // Data byte

  if (uart_midi_processor_buffer_limit == 0 || uart_midi_processor_buffer_index == 0 || uart_midi_processor_buffer_index >= uart_midi_processor_buffer_limit) {
    // No status byte to attach this data byte to (e.g. power-up mid-stream): ignore
    return ev;
  }

  uart_midi_processor_buffer[uart_midi_processor_buffer_index++] = byte;

  if (uart_midi_processor_buffer_index == uart_midi_processor_buffer_limit) {

    ev = uart_midi_processor_assemble();

    if (uart_midi_processor_buffer[0] < 0xF0) {
      // Clear the Data bytes from the buffer, but retain the Status byte,
      // since subsequent messages might be sent with Running Status.
      uart_midi_processor_buffer[1] = 0;
      uart_midi_processor_buffer[2] = 0;
      uart_midi_processor_buffer_index = 1;
    } else {
      // System Common Messages do not establish Running Status
      uart_midi_processor_clear();
    }
  }

  return ev;
}

uint8_t midi_rel_binoffset_to_twos_complement(uint8_t value) {
  // (value - 64) wrapped to 7 bits; identical to flipping bit 6
  return (uint8_t)((value ^ 0x40) & 0x7F);
}

struct uart_midi_event_packet uart_midi_convert_relative_cc(struct uart_midi_event_packet packet) {

  // Only complete 3-byte Control Change messages (status 0xB0..0xBF)
  if (packet.length != 3 || (packet.byte1 & 0xF0) != 0xB0) {
    return packet;
  }

  if (packet.byte2 < KNOT_REL_CC_FIRST || packet.byte2 > KNOT_REL_CC_LAST) {
    return packet;
  }

  packet.byte3 = midi_rel_binoffset_to_twos_complement(packet.byte3);

  return packet;
}
