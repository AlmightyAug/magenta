// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <magenta/types.h>


typedef enum {
    // Sent from client to host to simulate device connect/disconnect
    USB_VIRT_CONNECT,
    USB_VIRT_DISCONNECT,
    // Sent from either side to simulate packet send
    USB_VIRT_PACKET,
    USB_VIRT_PACKET_RESP,

} usb_virt_channel_cmd_t;

#define USB_VIRT_MAX_PACKET     (65536)
#define USB_VIRT_BUFFER_SIZE    (USB_VIRT_MAX_PACKET + sizeof(usb_virt_channel_cmd_t))

typedef struct {
    usb_virt_channel_cmd_t  cmd;
    // endpoint address for USB_VIRT_PACKET
    size_t                  data_length;
    mx_status_t             status;
    uintptr_t               cookie;
    uint8_t                 ep_addr;
    uint8_t                 padding[3]; // align data to 4 byte boundary
    uint8_t                 data[0];    // variable length packet data
} usb_virt_header_t;

mx_device_t* usb_virtual_hci_add(mx_device_t* parent, mx_handle_t channel_handle);
mx_device_t* usb_virtual_client_add(mx_device_t* parent, mx_handle_t channel_handle);
