#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct usb_midi_event_packet {
  uint8_t byte0;
  uint8_t byte1;
  uint8_t byte2;
  uint8_t byte3;
};

struct uart_midi_event_packet {
  uint8_t length;
  uint8_t byte1;
  uint8_t byte2;
  uint8_t byte3;
};

/**
 * @brief Convert USB midi event packets to 1-3 byte uart midi packets.
 * @param[in] usb_packet Contains the 4 byte usb event packet
 *
 * @return 1-3 byte uart_midi_event_packet containing the translated message
 */
struct uart_midi_event_packet usb_midi_to_uart(struct usb_midi_event_packet usb_packet);

/**
 * @brief Convert 1-3 byte uart midi packets to 4 byte USB midi event packets.
 * @param[in] uart_packet 1-3 byte uart_midi_event_packet
 *
 * @return The 4 byte usb event packet
 */
struct usb_midi_event_packet midi_uart_to_usb(struct uart_midi_event_packet uart_packet);

/**
 * @brief Determine whether a given byte represents a MIDI Real-Time Message or not
 * @param[in] byte MIDI byte
 *
 * @return true or false
 */
uint8_t uart_midi_is_byte_rtm(uint8_t byte);

/**
 * @brief Store next byte and return event packet once completed
 * @param[in] byte incoming 8-bit data chunk
 *
 * @return event packet if available, all zeros if not complete yet
 */
struct uart_midi_event_packet uart_midi_process_byte(uint8_t byte);

/**
 * @brief Reset the TRS/UART byte parser (clears SysEx state and Running Status)
 */
void uart_midi_processor_reset(void);

/*
 * Relative CC re-encoding
 *
 * Control Change messages whose controller number lies in
 * [KNOT_REL_CC_FIRST, KNOT_REL_CC_LAST] (any channel) are assumed to carry a
 * relative value in "binary offset" encoding (64 = 0, 65 = +1, 63 = -1, ...)
 * and are rewritten to 7-bit two's complement (0 = 0, 1 = +1, 127 = -1, ...).
 * All other messages pass through unchanged.
 */
#ifndef KNOT_REL_CC_FIRST
#define KNOT_REL_CC_FIRST 32
#endif
#ifndef KNOT_REL_CC_LAST
#define KNOT_REL_CC_LAST 49
#endif

/**
 * @brief Convert a binary offset relative value (64 = zero) to 7-bit two's complement
 * @param[in] value 7-bit binary offset value (0..127)
 *
 * @return 7-bit two's complement value (0..127)
 */
uint8_t midi_rel_binoffset_to_twos_complement(uint8_t value);

/**
 * @brief Re-encode relative CC values in the configured CC range from binary
 *        offset to two's complement. Non-matching packets are returned as-is.
 * @param[in] packet 1-3 byte uart_midi_event_packet
 *
 * @return the (possibly modified) packet
 */
struct uart_midi_event_packet uart_midi_convert_relative_cc(struct uart_midi_event_packet packet);

#ifdef __cplusplus
}
#endif
