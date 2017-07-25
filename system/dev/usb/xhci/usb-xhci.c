// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/driver.h>
#include <ddk/protocol/usb-hci.h>
#include <ddk/protocol/usb-xhci.h>
#include <ddk/protocol/usb.h>

#include <hw/reg.h>
#include <magenta/syscalls.h>
#include <magenta/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "xhci-device-manager.h"
#include "xhci-root-hub.h"
#include "xhci-util.h"
#include "xhci.h"

//#define TRACE 1
#include "xhci-debug.h"

#define MAX_SLOTS 255

mx_status_t xhci_add_device(xhci_t* xhci, int slot_id, int hub_address, int speed) {
    xprintf("xhci_add_new_device\n");

    if (!xhci->bus.ops) {
        printf("no bus device in xhci_add_device\n");
        return MX_ERR_INTERNAL;
    }

    return usb_bus_add_device(&xhci->bus, slot_id, hub_address, speed);
}

void xhci_remove_device(xhci_t* xhci, int slot_id) {
    xprintf("xhci_remove_device %d\n", slot_id);

    if (!xhci->bus.ops) {
        printf("no bus device in xhci_remove_device\n");
        return;
    }

    usb_bus_remove_device(&xhci->bus, slot_id);
}

static void xhci_set_bus_interface(void* ctx, usb_bus_interface_t* bus) {
    xhci_t* xhci = ctx;

    if (bus) {
        memcpy(&xhci->bus, bus, sizeof(xhci->bus));
        // wait until bus driver has started before doing this
        xhci_queue_start_root_hubs(xhci);
    } else {
        memset(&xhci->bus, 0, sizeof(xhci->bus));
    }
}

static size_t xhci_get_max_device_count(void* ctx) {
    xhci_t* xhci = ctx;
    // add one to allow device IDs to be 1-based
    return xhci->max_slots + XHCI_RH_COUNT + 1;
}

static mx_status_t xhci_enable_ep(void* ctx, uint32_t device_id,
                                  usb_endpoint_descriptor_t* ep_desc,
                                  usb_ss_ep_comp_descriptor_t* ss_comp_desc, bool enable) {
    xhci_t* xhci = ctx;
    return xhci_enable_endpoint(xhci, device_id, ep_desc, ss_comp_desc, enable);
}

static uint64_t xhci_get_frame(void* ctx) {
    xhci_t* xhci = ctx;
    return xhci_get_current_frame(xhci);
}

mx_status_t xhci_config_hub(void* ctx, uint32_t device_id, usb_speed_t speed,
                            usb_hub_descriptor_t* descriptor) {
    xhci_t* xhci = ctx;
    return xhci_configure_hub(xhci, device_id, speed, descriptor);
}

mx_status_t xhci_hub_device_added(void* ctx, uint32_t hub_address, int port,
                                  usb_speed_t speed) {
    xhci_t* xhci = ctx;
    return xhci_enumerate_device(xhci, hub_address, port, speed);
}

mx_status_t xhci_hub_device_removed(void* ctx, uint32_t hub_address, int port) {
    xhci_t* xhci = ctx;
    xhci_device_disconnected(xhci, hub_address, port);
    return MX_OK;
}

mx_status_t xhci_reset_ep(void* ctx, uint32_t device_id, uint8_t ep_address) {
    xhci_t* xhci = ctx;
    uint8_t ep_index = xhci_endpoint_index(ep_address);
    return xhci_reset_endpoint(xhci, device_id, ep_index);
}

size_t xhci_get_max_transfer_size(void* ctx, uint32_t device_id, uint8_t ep_address) {
    if (ep_address == 0) {
        // control requests have uint16 length field so we need to support UINT16_MAX
        // we require one setup, status and data event TRB in addition to data transfer TRBs
        // and subtract one more to account for the link TRB
        static_assert(PAGE_SIZE * (TRANSFER_RING_SIZE - 4) >= UINT16_MAX, "TRANSFER_RING_SIZE too small");
        return UINT16_MAX;
    }
    // non-control transfers consist of normal transfer TRBs plus one data event TRB
    // Subtract 2 to reserve a TRB for data event and to account for the link TRB
    return PAGE_SIZE * (TRANSFER_RING_SIZE - 2);
}

usb_hci_protocol_ops_t xhci_hci_protocol = {
    .set_bus_interface = xhci_set_bus_interface,
    .get_max_device_count = xhci_get_max_device_count,
    .enable_endpoint = xhci_enable_ep,
    .get_current_frame = xhci_get_frame,
    .configure_hub = xhci_config_hub,
    .hub_device_added = xhci_hub_device_added,
    .hub_device_removed = xhci_hub_device_removed,
    .reset_endpoint = xhci_reset_ep,
    .get_max_transfer_size = xhci_get_max_transfer_size,
};

static void xhci_iotxn_queue(void* ctx, iotxn_t* txn) {
    xhci_t* xhci = ctx;
    usb_protocol_data_t* data = iotxn_pdata(txn, usb_protocol_data_t);
    mx_status_t status;

    if (txn->length > xhci_get_max_transfer_size(xhci->mxdev, data->device_id, data->ep_address)) {
        status = MX_ERR_INVALID_ARGS;
    } else {
        status = xhci_queue_transfer(xhci, txn);
    }

    if (status != MX_OK && status != MX_ERR_BUFFER_TOO_SMALL) {
        iotxn_complete(txn, status, 0);
    }
}

static void xhci_unbind(void* ctx) {
    xhci_t* xhci = ctx;
    xprintf("xhci_unbind\n");

    device_remove(xhci->mxdev);
}

static void xhci_release(void* ctx) {
    xhci_t* xhci = ctx;
    mx_handle_close(xhci->irq_handle);
    free(xhci);
}

static mx_protocol_device_t xhci_device_proto = {
    .version = DEVICE_OPS_VERSION,
    .iotxn_queue = xhci_iotxn_queue,
    .unbind = xhci_unbind,
    .release = xhci_release,
};

static int xhci_irq_thread(void* arg) {
    xhci_t* xhci = (xhci_t*)arg;
    xprintf("xhci_irq_thread start\n");

    // xhci_start will block, so do this part here instead of in usb_xhci_bind
    xhci_start(xhci);

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "xhci",
        .ctx = xhci,
        .ops = &xhci_device_proto,
        .proto_id = MX_PROTOCOL_USB_HCI,
        .proto_ops = &xhci_hci_protocol,
    };

    mx_status_t status = device_add(xhci->parent, &args, &xhci->mxdev);
    if (status != MX_OK) {
        free(xhci);
        return status;
    }

    // TODO(johngro) : See MG-940.  Get rid of this.  No matter how we approach
    // the problem of realtime latency in magenta, clearly not all XHCI
    // transactions are high priority.  At the very least, we need to split this
    // system so that there are at least two completers bound to two IRQs, and
    // that only realtime transactions use the completer which ends up getting
    // realtime latency guarantees.
    mx_thread_set_priority(24 /* HIGH_PRIORITY in LK */);

    while (1) {
        mx_status_t wait_res;

        wait_res = mx_interrupt_wait(xhci->irq_handle);
        if (wait_res != MX_OK) {
            printf("unexpected pci_wait_interrupt failure (%d)\n", wait_res);
            mx_interrupt_complete(xhci->irq_handle);
            break;
        }

        mx_interrupt_complete(xhci->irq_handle);
        xhci_handle_interrupt(xhci);
    }
    xprintf("xhci_irq_thread done\n");
    return 0;
}

static mx_status_t usb_xhci_bind(void* ctx, mx_device_t* dev, void** cookie) {
    mx_handle_t irq_handle = MX_HANDLE_INVALID;
    xhci_t* xhci = NULL;
    mx_status_t status;

    usb_xhci_protocol_t xhci_proto;
    if (device_get_protocol(dev, MX_PROTOCOL_USB_XHCI, &xhci_proto)) {
        status = MX_ERR_NOT_SUPPORTED;
        goto error_return;
    }

    xhci = calloc(1, sizeof(xhci_t));
    if (!xhci) {
        status = MX_ERR_NO_MEMORY;
        goto error_return;
    }

    void* mmio;
    uint64_t mmio_len;
    /*
     * eXtensible Host Controller Interface revision 1.1, section 5, xhci
     * should only use BARs 0 and 1. 0 for 32 bit addressing, and 0+1 for 64 bit addressing.
     */
    status = usb_xhci_get_mmio(&xhci_proto, &mmio, &mmio_len);
    if (status != MX_OK) {
        printf("usb_xhci_bind: usb_xhci_get_mmio failed\n");
        goto error_return;
    }

    // register for interrupts
    status = usb_xhci_get_interrupt(&xhci_proto, 0, &irq_handle);
    if (status != MX_OK) {
        printf("usb_xhci_bind: usb_xhci_get_interrupt failed %d\n", status);
        goto error_return;
    }

    xhci->irq_handle = irq_handle;
    xhci->legacy_irq_mode = usb_xhci_legacy_irq_mode(&xhci_proto);

    // stash this here for the startup thread to call device_add() with
    xhci->parent = dev;

    status = xhci_init(xhci, mmio);
    if (status != MX_OK) {
        goto error_return;
    }

    thrd_t thread;
    thrd_create_with_name(&thread, xhci_irq_thread, xhci, "xhci_irq_thread");
    thrd_detach(thread);

    return MX_OK;

error_return:
    if (xhci) {
        free(xhci);
    }
    if (irq_handle != MX_HANDLE_INVALID) {
        mx_handle_close(irq_handle);
    }
    return status;
}

static mx_driver_ops_t xhci_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = usb_xhci_bind,
};

// clang-format off
MAGENTA_DRIVER_BEGIN(usb_xhci, xhci_driver_ops, "magenta", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, MX_PROTOCOL_USB_XHCI),
MAGENTA_DRIVER_END(usb_xhci)
