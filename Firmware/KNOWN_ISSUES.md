# Known issues in the Knot firmware

Found during a code review on 2026-11-20, while adding the relative CC
(binary offset → two's complement) conversion. None of these are fixed yet.
Line numbers refer to the code as of commit `c703fc2`.

Items marked **confirmed** were reproduced by running
`components/knot_midi_translator/knot_midi_translator.c` on the host with test
input, the first one under AddressSanitizer.

## Priority

| # | Issue | Severity | Status |
|---|-------|----------|--------|
| 1 | TRS parser buffer overrun on data bytes with no status byte | High | Confirmed |
| 2 | System Common messages from TRS lost or mis-encoded | Medium | Confirmed |
| 3 | USB device with a single MIDI endpoint causes reboot loop | Medium | From code reading |
| 4 | USB output stalls after hot-plug; transfer buffer reuse | Medium | From code reading |
| 5 | MIDI queues not thread-safe and overflow silently | Low–Medium | From code reading |
| 6 | Minor issues | Low | From code reading |

---

## 1. TRS parser buffer overrun (confirmed)

**Where:** `components/knot_midi_translator/knot_midi_translator.c`,
`uart_midi_process_byte()`, data-byte branch (~line 226).

**What happens:** `uart_midi_processor_buffer` is 3 bytes long. When no status
byte has been seen, `uart_midi_processor_buffer_limit` is 0. This is the case
at power-up, and again after a SysEx end, which clears the buffer and sets the
limit to 0. In that state each incoming data byte (< 0x80) is stored at
`buffer[index++]`. The check `index == limit` never matches, so the index keeps
growing and writes run past the end of the array into neighbouring globals.

**When it triggers:** Knot powers up or the TRS cable is plugged in while a
device is already sending (running status, active sensing data, etc.). It also
triggers when stray data bytes follow a SysEx.

**Effect:** memory corruption. Possible results are garbled MIDI, a stuck
parser or a crash.

**Repro:** feed the bytes `40 41 42 43 44` to `uart_midi_process_byte()` right
after reset. ASan reports a global-buffer-overflow on the first byte.

**Fix:** ignore data bytes when `uart_midi_processor_buffer_limit == 0` (no
running status). As a safeguard, never write when `index >= 3`.

## 2. System Common messages from TRS lost or mis-encoded (confirmed)

**Where:** `uart_midi_process_byte()` (status-byte branch, ~line 216) and
`midi_uart_to_usb()` (~line 127).

**What happens:**
- All non-real-time status bytes other than 0xCn/0xDn get a buffer limit of 3.
  System Common messages have other lengths:
  - `F1` (MTC quarter frame) and `F3` (Song Select) have 1 data byte, so they
    never complete and are **dropped**.
  - `F6` (Tune Request) has 0 data bytes and is **dropped**.
- `F2` (Song Position Pointer) is parsed correctly. However,
  `midi_uart_to_usb()` derives the CIN as `status >> 4` = `0xF` ("single
  byte"), so the USB host receives only the `F2` byte and the position is
  **lost**. The correct CINs are 0x2 (2-byte System Common), 0x3 (3-byte) and
  0x5 (1-byte).
- System Common messages should also cancel running status. Currently they are
  kept as the running status.

**Effect:** MTC and Song Position Pointer sync from TRS to USB do not work.

**Fix:** set the per-status lengths (F1/F3 → 2, F2 → 3, F6 → 1, emitted
immediately) and clear running status after System Common. In
`midi_uart_to_usb()`, map F1/F3 → CIN 2, F2 → CIN 3, F6 → CIN 5. Add unit
tests alongside the existing ones in `host_test/main.c`.

## 3. Single-endpoint USB MIDI devices cause a reboot loop

**Where:** `components/knot_midi_usb/knot_midi_usb.c`,
`midi_find_intf_and_ep_desc()` (~lines 144 and 192) and
`action_get_config_desc()` (~line 355).

**What happens:** the code always parses endpoint indices 0 and 1 and
`assert(this_ep)`s each one. A MIDI-streaming interface with only an IN
endpoint, such as a send-only controller, fails the assert and aborts, so Knot
reboots repeatedly while that device is plugged in. If an interface had two
endpoints in the same direction, `out_ep` would stay NULL and be dereferenced.

**Fix:** loop over `bNumEndpoints` and drop the assert. Treat a missing OUT
endpoint as "input-only": skip the OUT transfer setup and make
`knot_midi_usb_send_packet()` a no-op.

## 4. USB output stalls after hot-plug; transfer buffer reuse

**Where:** `knot_midi_usb.c`: `usb_out_ready` (~line 279),
`knot_midi_usb_send_packet()`, `aciton_close_dev()` (~line 418) and
`action_get_config_desc()`.

**What happens:**
- `knot_midi_usb_send_packet()` sets `usb_out_ready = 0` before submitting. Only
  `out_transfer_cb` sets it back to 1. If the submit fails, or the device is
  unplugged and the callback never arrives, the flag stays 0. Nothing resets it
  on reconnect, so `knot_midi_usb_tx_task` never sends again and **TRS → USB
  output stops until a power cycle.**
- `aciton_close_dev()` sets `out_transfer = NULL` without
  `usb_host_transfer_free()`, which leaks the transfer on every disconnect.
- `in_transfer` is allocated once, with the first device's IN max packet size,
  and reused. A later device with a larger MPS sets `num_bytes` above the
  buffer size, so the submit fails and that device delivers no MIDI.
- `in_transfer_cb` doesn't check `transfer->status`. It parses data and
  resubmits even on error, cancellation or no-device.

**Fix:** on close, free both transfers, set them to NULL and reset
`usb_out_ready = 1`. Allocate the transfers per device. Set `usb_out_ready = 1`
again when a submit returns an error. Check the status in `in_transfer_cb`.

## 5. MIDI queues not thread-safe; silent overflow

**Where:** `components/knot_midi_queue/knot_midi_queue.c`.

**What happens:**
- The queues are plain ring buffers with no locking or `volatile`.
  `trsout_push` is called from both the USB IN callback (class driver task,
  priority 4) and the UART RX task (priority 0). `usbout_pop` is called from
  both `out_transfer_cb` and `knot_midi_usb_tx_task`. Preemption mid-operation
  can duplicate or lose events.
- `push` doesn't check for a full queue. When 50 events are pending, the write
  index catches up to the read index, the queue appears **empty**, and every
  queued event is dropped. A large SysEx dump can do this.

**Fix:** use FreeRTOS queues (`xQueueSend` / `xQueueReceive`), or protect push
and pop with a `portMUX` critical section, and refuse or count pushes when the
queue is full.

## 6. Minor issues

- **Unterminated SysEx:** a status byte (other than real-time) received during
  SysEx should end the SysEx. Currently it is stored as SysEx data and the
  parser stays in SysEx mode, chunking the following bytes in threes, which
  misframes 2-byte messages and breaks running status.
- **Non-IAD interface detection** (`midi_find_intf_and_ep_desc()`, else
  branch): this path uses the count of interface descriptors (`intf_idx`) as
  the interface number. That is wrong when earlier interfaces have alternate
  settings, which is common on audio+MIDI devices. Use `bInterfaceNumber` and
  `bAlternateSetting` as the IAD branch does.
- **Busy-wait tasks:** the UART RX/TX and USB RX/TX tasks spin with
  `portYIELD()` and never block. `knot_midi_uart_rx_task` also calls
  `uart_read_bytes(..., length, ...)` with no cap at `RD_BUF_SIZE`. That is
  safe today only because the driver's RX buffer (2 × `BUF_SIZE`) is drained
  often.
- **`usb_in_ready`** is never set to 1, so `try_start_in_transfer()` and
  `knot_midi_usb_rx_task` are dead code.
- **`grid_platform_printf()`** (`main/midi_host_fw.c`) passes the formatted
  text to `ets_printf()` as a format string. Any `%` in the output is
  interpreted again. Use `ets_printf("%s", temp)`.

---

## Build notes (fork `neon-xx/knot_rel-2s`)

To get CI building again in November 2026, these were pinned in
`Firmware/main/idf_component.yml`:

- `sukuwc/grid_*` → `<2026.2.270000` (resolved to 2026.2.251637). Version
  2026.11.x requires a `tinyusb` component that Knot doesn't provide.
- `joltwallet/littlefs` → `<1.23.0`. Version 1.23.1 gates the raw LittleFS API
  behind `CONFIG_LITTLEFS_EXPOSE_RAW_API`.

Two other fixes were needed:

- The firmware workflow grants `contents: write` and `packages: write`.
- The Docker image uses `pip install --ignore-installed`.

Before un-pinning, check that newer versions build.
