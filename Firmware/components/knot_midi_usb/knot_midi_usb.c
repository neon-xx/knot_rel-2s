/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "knot_midi_usb.h"

#include "esp_log.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "grid_led.h"
#include "usb/usb_host.h"
#include <stdlib.h>
#include <string.h>

#include "knot_midi_queue.h"
#include "knot_midi_translator.h"
#include "knot_midi_uart.h"

#include "rom/ets_sys.h" // For ets_printf

#define CLIENT_NUM_EVENT_MSG 5

#define ACTION_OPEN_DEV 0x01
#define ACTION_GET_DEV_INFO 0x02
#define ACTION_GET_DEV_DESC 0x04
#define ACTION_GET_CONFIG_DESC 0x08
#define ACTION_GET_STR_DESC 0x10
#define ACTION_CLOSE_DEV 0x20
#define ACTION_EXIT 0x40

static const char* TAG = "KNOT_MIDI_USB";

static void client_event_cb(const usb_host_client_event_msg_t* event_msg, void* arg) {

  class_driver_t* driver_obj = (class_driver_t*)arg;

  ets_printf("client event %d\r\n", event_msg->event);

  switch (event_msg->event) {
  case USB_HOST_CLIENT_EVENT_NEW_DEV:
    if (driver_obj->dev_addr == 0) {
      driver_obj->dev_addr = event_msg->new_dev.address;
      // Open the device next
      driver_obj->actions |= ACTION_OPEN_DEV;
    }
    break;
  case USB_HOST_CLIENT_EVENT_DEV_GONE:
    if (driver_obj->dev_hdl != NULL) {
      // Cancel any other actions and close the device next
      driver_obj->actions = ACTION_CLOSE_DEV;
    }
    break;
  default:
    // Should never occur
    abort();
  }
}

static void action_open_dev(class_driver_t* driver_obj) {
  assert(driver_obj->dev_addr != 0);
  ESP_LOGI(TAG, "Opening device at address %d", driver_obj->dev_addr);
  ESP_ERROR_CHECK(usb_host_device_open(driver_obj->client_hdl, driver_obj->dev_addr, &driver_obj->dev_hdl));
  // Get the device's information next
  driver_obj->actions &= ~ACTION_OPEN_DEV;
  driver_obj->actions |= ACTION_GET_DEV_INFO;
}

static void action_get_info(class_driver_t* driver_obj) {
  assert(driver_obj->dev_hdl != NULL);
  ESP_LOGI(TAG, "Getting device information");
  usb_device_info_t dev_info;
  ESP_ERROR_CHECK(usb_host_device_info(driver_obj->dev_hdl, &dev_info));
  ESP_LOGI(TAG, "\t%s speed", (dev_info.speed == USB_SPEED_LOW) ? "Low" : "Full");
  ESP_LOGI(TAG, "\tbConfigurationValue %d", dev_info.bConfigurationValue);
  // Todo: Print string descriptors

  // Get the device descriptor next
  driver_obj->actions &= ~ACTION_GET_DEV_INFO;
  driver_obj->actions |= ACTION_GET_DEV_DESC;
}

unsigned long loopcounter = 0;

#define USB_SUBCLASS_MIDISTREAMING 0x03

static bool midi_ep_is_bulk_or_interrupt(const usb_ep_desc_t* ep) {
  uint8_t type = ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;
  return type == USB_BM_ATTRIBUTES_XFER_BULK || type == USB_BM_ATTRIBUTES_XFER_INT;
}

/*
 * Find the first MIDI Streaming interface (any device class, any alternate
 * setting) that has at least one bulk/interrupt endpoint, and return its
 * interface number, alternate setting and IN/OUT endpoints. Either endpoint
 * may be NULL if the interface only has one direction.
 */
static esp_err_t midi_find_intf_and_ep_desc(class_driver_t* driver_obj, const usb_ep_desc_t** in_ep, const usb_ep_desc_t** out_ep, int* intf_num, int* intf_alt) {

  const usb_config_desc_t* config_desc;
  ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(driver_obj->dev_hdl, &config_desc));

  *in_ep = NULL;
  *out_ep = NULL;

  int desc_offset = 0;
  const usb_standard_desc_t* this_desc = (const usb_standard_desc_t*)config_desc;

  while (1) {

    this_desc = usb_parse_next_descriptor_of_type(this_desc, config_desc->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &desc_offset);

    if (this_desc == NULL) {
      ESP_LOGI(TAG, "MIDI INTERFACE NOT FOUND");
      return ESP_ERR_NOT_FOUND;
    }

    const usb_intf_desc_t* intf_desc = (const usb_intf_desc_t*)this_desc;

    ESP_LOGI(TAG, "Interface %d alt %d class %d %d %d, %d endpoints", intf_desc->bInterfaceNumber, intf_desc->bAlternateSetting, intf_desc->bInterfaceClass, intf_desc->bInterfaceSubClass,
             intf_desc->bInterfaceProtocol, intf_desc->bNumEndpoints);

    if (intf_desc->bInterfaceClass != USB_CLASS_AUDIO || intf_desc->bInterfaceSubClass != USB_SUBCLASS_MIDISTREAMING) {
      continue;
    }

    const usb_ep_desc_t* found_in = NULL;
    const usb_ep_desc_t* found_out = NULL;

    for (int i = 0; i < intf_desc->bNumEndpoints; i++) {
      int ep_offset = desc_offset;
      const usb_ep_desc_t* ep = usb_parse_endpoint_descriptor_by_index(intf_desc, i, config_desc->wTotalLength, &ep_offset);
      if (ep == NULL || !midi_ep_is_bulk_or_interrupt(ep)) {
        continue;
      }
      if (USB_EP_DESC_GET_EP_DIR(ep)) {
        if (found_in == NULL) {
          found_in = ep;
        }
      } else {
        if (found_out == NULL) {
          found_out = ep;
        }
      }
    }

    if (found_in == NULL && found_out == NULL) {
      continue; // e.g. alternate setting 0 without endpoints
    }

    *in_ep = found_in;
    *out_ep = found_out;
    *intf_num = intf_desc->bInterfaceNumber;
    *intf_alt = intf_desc->bAlternateSetting;

    ESP_LOGI(TAG, "MIDI INTERFACE FOUND %d alt %d, IN ep %02x, OUT ep %02x", *intf_num, *intf_alt, found_in ? found_in->bEndpointAddress : 0, found_out ? found_out->bEndpointAddress : 0);
    return ESP_OK;
  }
}

static usb_transfer_t* out_transfer;

static usb_transfer_t* in_transfer;

static bool is_transfer_completed(usb_transfer_t* transfer) {
  bool completed = false;

  switch (transfer->status) {
  case USB_TRANSFER_STATUS_COMPLETED:
    completed = true;
    break;
  case USB_TRANSFER_STATUS_NO_DEVICE: // User is notified about device disconnection from usb_event_cb
  case USB_TRANSFER_STATUS_CANCELED:
    break;
  case USB_TRANSFER_STATUS_ERROR:
  case USB_TRANSFER_STATUS_TIMED_OUT:
  case USB_TRANSFER_STATUS_STALL:
  case USB_TRANSFER_STATUS_OVERFLOW:
  case USB_TRANSFER_STATUS_SKIPPED:
  default:
  }
  return completed;
}

volatile uint8_t DRAM_ATTR usb_in_ready = 0;

static portMUX_TYPE in_mutex = portMUX_INITIALIZER_UNLOCKED;
#define CDC_ACM_ENTER_CRITICAL() portENTER_CRITICAL(&in_mutex)
#define CDC_ACM_EXIT_CRITICAL() portEXIT_CRITICAL(&in_mutex)
esp_err_t try_start_in_transfer(void) {
  CDC_ACM_ENTER_CRITICAL();

  if (usb_in_ready == 0) {

    CDC_ACM_EXIT_CRITICAL();
    return -1;
  }

  esp_err_t err = usb_host_transfer_submit(in_transfer);
  usb_in_ready = 0;
  CDC_ACM_EXIT_CRITICAL();

  // printf("S\n");
  return err;
}

static volatile bool DRAM_ATTR usb_in_active = false;

static void in_transfer_cb(usb_transfer_t* in_transfer) {

  if (in_transfer->status == USB_TRANSFER_STATUS_NO_DEVICE || in_transfer->status == USB_TRANSFER_STATUS_CANCELED) {
    // device is gone or endpoint was flushed: do not resubmit
    usb_in_active = false;
    return;
  }

  if (in_transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
    // error / timeout / stall: no valid data, keep listening
    if (usb_in_active && usb_host_transfer_submit(in_transfer) != ESP_OK) {
      usb_in_active = false;
    }
    return;
  }

  for (int i = 0; i < in_transfer->actual_num_bytes; i += 4) {

    if (i + 4 > in_transfer->actual_num_bytes) {
      break;
    }

    struct usb_midi_event_packet evt = {
        .byte0 = in_transfer->data_buffer[i + 0],
        .byte1 = in_transfer->data_buffer[i + 1],
        .byte2 = in_transfer->data_buffer[i + 2],
        .byte3 = in_transfer->data_buffer[i + 3],
    };

    knot_midi_queue_trsout_push(uart_midi_convert_relative_cc(usb_midi_to_uart(evt)));
  }

  if (usb_in_active && usb_host_transfer_submit(in_transfer) != ESP_OK) {
    usb_in_active = false;
  }
}

volatile uint8_t DRAM_ATTR usb_out_ready = 1;

// true while a device with an OUT endpoint is open and out_transfer is set up for it
static volatile bool DRAM_ATTR usb_out_connected = false;

static void IRAM_ATTR out_transfer_cb(usb_transfer_t* out_transfer) {
  // ets_printf("Ready\n");
  usb_out_ready = 1;

  if (!usb_out_connected) {
    return;
  }

  struct usb_midi_event_packet usb_ev = {0, 0, 0, 0};

  int status = knot_midi_queue_usbout_pop(&usb_ev);

  if (status == 1) {
    return;
  }

  knot_midi_usb_send_packet(usb_ev);
}

uint8_t IRAM_ATTR knot_midi_usb_out_isready(void) { return usb_out_ready; }

int IRAM_ATTR knot_midi_usb_send_packet(struct usb_midi_event_packet ev) {

  if (out_transfer == NULL || !usb_out_connected) {
    return 1;
  }

  out_transfer->num_bytes = 4;

  out_transfer->data_buffer[0] = ev.byte0;
  out_transfer->data_buffer[1] = ev.byte1;
  out_transfer->data_buffer[2] = ev.byte2;
  out_transfer->data_buffer[3] = ev.byte3;
  out_transfer->callback = out_transfer_cb;

  usb_out_ready = 0;
  esp_err_t err = usb_host_transfer_submit(out_transfer);

  if (err != ESP_OK) {
    // no callback will follow a failed submit: release the busy flag
    usb_out_ready = 1;
  }

  return err;
}

static void action_get_dev_desc(class_driver_t* driver_obj) {
  assert(driver_obj->dev_hdl != NULL);
  ESP_LOGI(TAG, "Getting device descriptor");
  const usb_device_desc_t* dev_desc;
  ESP_ERROR_CHECK(usb_host_get_device_descriptor(driver_obj->dev_hdl, &dev_desc));
  usb_print_device_descriptor(dev_desc);
  // Get the device's config descriptor next
  driver_obj->actions &= ~ACTION_GET_DEV_DESC;
  driver_obj->actions |= ACTION_GET_CONFIG_DESC;
}

static void action_get_config_desc(class_driver_t* driver_obj) {
  assert(driver_obj->dev_hdl != NULL);
  ESP_LOGI(TAG, "Getting config descriptor");
  const usb_config_desc_t* config_desc;
  ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(driver_obj->dev_hdl, &config_desc));
  usb_print_config_descriptor(config_desc, NULL);

  const usb_ep_desc_t* in_ep = NULL;
  const usb_ep_desc_t* out_ep = NULL;
  int intf_num = -1;
  int intf_alt = 0;

  if (midi_find_intf_and_ep_desc(driver_obj, &in_ep, &out_ep, &intf_num, &intf_alt) != ESP_OK) {
    ESP_LOGI(TAG, "Iface not found, try later!");
    vTaskDelay(pdMS_TO_TICKS(1000));
    return;
  }

  esp_err_t err = usb_host_interface_claim(driver_obj->client_hdl, driver_obj->dev_hdl, intf_num, intf_alt);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Claiming interface %d alt %d failed: %s", intf_num, intf_alt, esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(1000));
    return;
  }

  ESP_LOGI(TAG, "Claimed interface %d alt %d", intf_num, intf_alt);
  driver_obj->claimed_interface = intf_num;
  driver_obj->in_ep_addr = in_ep ? in_ep->bEndpointAddress : 0;
  driver_obj->out_ep_addr = out_ep ? out_ep->bEndpointAddress : 0;

  if (out_ep != NULL) {
    // The 4 byte OUT transfer is allocated once and reused for every device
    if (out_transfer == NULL) {
      ESP_ERROR_CHECK(usb_host_transfer_alloc(4, 0, &out_transfer));
    }
    memset(out_transfer->data_buffer, 0xAA, 4);
    out_transfer->num_bytes = 4;
    out_transfer->device_handle = driver_obj->dev_hdl;
    out_transfer->bEndpointAddress = out_ep->bEndpointAddress;
    out_transfer->callback = out_transfer_cb;
    out_transfer->context = (void*)driver_obj;
    usb_out_ready = 1;
    usb_out_connected = true;
    ESP_LOGI(TAG, "Setup OUT transfer on ep %02x", out_ep->bEndpointAddress);
  } else {
    ESP_LOGI(TAG, "Device has no MIDI OUT endpoint (input only)");
  }

  if (in_ep != NULL) {
    int mps = USB_EP_DESC_GET_MPS(in_ep);

    // (Re)allocate if the previous device's buffer is too small. The previous
    // IN transfer is no longer in flight: in_transfer_cb does not resubmit
    // after NO_DEVICE / CANCELED.
    if (in_transfer != NULL && in_transfer->data_buffer_size < (size_t)mps) {
      usb_host_transfer_free(in_transfer);
      in_transfer = NULL;
    }
    if (in_transfer == NULL) {
      ESP_ERROR_CHECK(usb_host_transfer_alloc(mps, 0, &in_transfer));
    }

    in_transfer->num_bytes = mps;
    in_transfer->device_handle = driver_obj->dev_hdl;
    in_transfer->bEndpointAddress = in_ep->bEndpointAddress;
    in_transfer->timeout_ms = 5000;
    in_transfer->callback = in_transfer_cb;
    in_transfer->context = (void*)driver_obj;

    ESP_LOGI(TAG, "Start IN transfer on ep %02x", in_ep->bEndpointAddress);
    usb_in_active = true;
    if (usb_host_transfer_submit(in_transfer) != ESP_OK) {
      usb_in_active = false;
      ESP_LOGE(TAG, "IN transfer submit failed");
    }
  } else {
    ESP_LOGI(TAG, "Device has no MIDI IN endpoint (output only)");
  }

  // Get the device's string descriptors next
  driver_obj->actions &= ~ACTION_GET_CONFIG_DESC;
  driver_obj->actions |= ACTION_GET_STR_DESC;
}

static void action_get_str_desc(class_driver_t* driver_obj) {

  if (loopcounter == 0) {
    assert(driver_obj->dev_hdl != NULL);
    usb_device_info_t dev_info;
    ESP_ERROR_CHECK(usb_host_device_info(driver_obj->dev_hdl, &dev_info));
    if (dev_info.str_desc_manufacturer) {
      ESP_LOGI(TAG, "Getting Manufacturer string descriptor");
      usb_print_string_descriptor(dev_info.str_desc_manufacturer);
    }
    if (dev_info.str_desc_product) {
      ESP_LOGI(TAG, "Getting Product string descriptor");
      usb_print_string_descriptor(dev_info.str_desc_product);
    }
    if (dev_info.str_desc_serial_num) {
      ESP_LOGI(TAG, "Getting Serial Number string descriptor");
      usb_print_string_descriptor(dev_info.str_desc_serial_num);
    }
  }

  // if (loopcounter%10 == 1){

  //     usb_host_transfer_submit(transfer);

  // }

  loopcounter++;

  // Nothing to do until the device disconnects
  // driver_obj->actions &= ~ACTION_GET_STR_DESC;
}

static void midi_ep_stop(class_driver_t* driver_obj, uint8_t ep_addr) {
  if (ep_addr == 0) {
    return;
  }
  // Errors are expected here when the device is already gone; ignore them.
  usb_host_endpoint_halt(driver_obj->dev_hdl, ep_addr);
  usb_host_endpoint_flush(driver_obj->dev_hdl, ep_addr);
  usb_host_endpoint_clear(driver_obj->dev_hdl, ep_addr);
}

static void aciton_close_dev(class_driver_t* driver_obj) {

  ESP_LOGI(TAG, "action_close_dev");

  // Stop USB output first so callbacks / the TX task do not submit anymore
  usb_out_connected = false;
  usb_in_active = false;

  if (driver_obj->dev_hdl != NULL) {

    midi_ep_stop(driver_obj, driver_obj->in_ep_addr);
    midi_ep_stop(driver_obj, driver_obj->out_ep_addr);

    // Let the flushed transfers' callbacks run before releasing the interface
    usb_host_client_handle_events(driver_obj->client_hdl, 0);

    if (driver_obj->claimed_interface != -1) {
      ESP_LOGI(TAG, "release interface %d", driver_obj->claimed_interface);
      usb_host_interface_release(driver_obj->client_hdl, driver_obj->dev_hdl, driver_obj->claimed_interface);
      driver_obj->claimed_interface = -1;
    }

    ESP_LOGI(TAG, "close");
    ESP_ERROR_CHECK(usb_host_device_close(driver_obj->client_hdl, driver_obj->dev_hdl));
  }

  driver_obj->dev_hdl = NULL;
  driver_obj->dev_addr = 0;
  driver_obj->in_ep_addr = 0;
  driver_obj->out_ep_addr = 0;

  // Nothing is in flight anymore: the next device starts with a free OUT path
  usb_out_ready = 1;

  ESP_ERROR_CHECK(usb_host_client_deregister(driver_obj->client_hdl));

  driver_obj->client_hdl = NULL;

  ESP_LOGI(TAG, "close: %lx %lx", (long unsigned int)driver_obj->dev_hdl, (long unsigned int)driver_obj->client_hdl);

  // We need to exit the event handler loop
  driver_obj->actions &= ~ACTION_CLOSE_DEV;
  driver_obj->actions |= ACTION_OPEN_DEV;

  // driver_obj->actions |= ACTION_EXIT;
}

void class_driver_task(void* arg) {
  SemaphoreHandle_t signaling_sem = (SemaphoreHandle_t)arg;
  class_driver_t driver_obj = {0};
  driver_obj.claimed_interface = -1;

  in_transfer = NULL;
  out_transfer = NULL;

  // Wait until daemon task has installed USB Host Library
  xSemaphoreTake(signaling_sem, portMAX_DELAY);

  grid_alert_all_set(&grid_led_state, 100, 100, 100, -1);

  while (1) {

    // ESP_LOGI(TAG, "actions: %ld, loopcounter: %ld", driver_obj.actions, loopcounter);

    while (driver_obj.client_hdl == NULL) {

      ESP_LOGI(TAG, "Try Registering Client");
      usb_host_client_config_t client_config = {
          .is_synchronous = false, // Synchronous clients currently not supported. Set this to false
          .max_num_event_msg = CLIENT_NUM_EVENT_MSG,
          .async =
              {
                  .client_event_callback = client_event_cb,
                  .callback_arg = (void*)&driver_obj,
              },
      };
      ESP_ERROR_CHECK(usb_host_client_register(&client_config, &driver_obj.client_hdl));

      vTaskDelay(pdMS_TO_TICKS(1));
    }

    usb_host_client_handle_events(driver_obj.client_hdl, 10);

    if (driver_obj.actions & ACTION_OPEN_DEV) {
      if (driver_obj.dev_addr != 0) {

        // stop waiting pulse animation
        grid_alert_all_set(&grid_led_state, 0, 0, 0, 0);

        action_open_dev(&driver_obj);
      } else {
        ESP_LOGI(TAG, "Pending");
      }
    }
    if (driver_obj.actions & ACTION_GET_DEV_INFO) {
      action_get_info(&driver_obj);
    }
    if (driver_obj.actions & ACTION_GET_DEV_DESC) {
      action_get_dev_desc(&driver_obj);
      // led_connect_effect_start();
    }
    if (driver_obj.actions & ACTION_GET_CONFIG_DESC) {
      action_get_config_desc(&driver_obj);
    }
    if (driver_obj.actions & ACTION_GET_STR_DESC) {
      action_get_str_desc(&driver_obj);
    }
    if (driver_obj.actions & ACTION_CLOSE_DEV) {
      aciton_close_dev(&driver_obj);
      grid_alert_all_set(&grid_led_state, 100, 100, 100, -1);
    }
    if (driver_obj.actions & ACTION_EXIT) {
      break;
    }
    portYIELD();
  }

  ESP_LOGI(TAG, "Deregistering Client");
  ESP_ERROR_CHECK(usb_host_client_deregister(driver_obj.client_hdl));

  // Wait to be deleted
  xSemaphoreGive(signaling_sem);
  vTaskSuspend(NULL);
}

void knot_midi_usb_rx_task(void* arg) {
  while (1) {

    try_start_in_transfer();

    portYIELD();
  }
}

void knot_midi_usb_tx_task(void* arg) {

  while (1) {

    if (knot_midi_usb_out_isready()) {

      struct usb_midi_event_packet usb_midi_ev = {0};

      if (0 == knot_midi_queue_usbout_pop(&usb_midi_ev)) {

        int status = knot_midi_usb_send_packet(usb_midi_ev);

        if (status == 0) {
          // ets_printf("USB OUT: %d %d %d %d", usb_midi_ev.byte0, usb_midi_ev.byte1, usb_midi_ev.byte2, usb_midi_ev.byte3);
        } else if (status == 1) {
          // ets_printf("USB not connected");
        } else {
          // ets_printf("USB error: %d", status);
        }
      }
    }

    portYIELD();
  }
}
