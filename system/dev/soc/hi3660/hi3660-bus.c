// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/binding.h>
#include <ddk/protocol/gpio.h>

#include <magenta/process.h>
#include <magenta/syscalls.h>
#include <magenta/assert.h>

#include "hi3660-bus.h"
#include "pl061.h"

static pl061_gpios_t* find_gpio(hi3660_bus_t* bus, unsigned pin) {
    pl061_gpios_t* gpios;
    // TODO(voydanoff) consider using a fancier data structure here
    list_for_every_entry(&bus->gpios, gpios, pl061_gpios_t, node) {
        if (pin >= gpios->gpio_start && pin < gpios->gpio_start + gpios->gpio_count) {
            return gpios;
        }
    }
    return NULL;
}

static mx_status_t hi3660_gpio_config(void* ctx, unsigned pin, gpio_config_flags_t flags) {
    hi3660_bus_t* bus = ctx;
    pl061_gpios_t* gpios = find_gpio(bus, pin);
    if (!gpios) {
        return MX_ERR_INVALID_ARGS;
    }
    return pl061_proto_ops.config(gpios, pin, flags);
}

static mx_status_t hi3660_gpio_read(void* ctx, unsigned pin, unsigned* out_value) {
    hi3660_bus_t* bus = ctx;
    pl061_gpios_t* gpios = find_gpio(bus, pin);
    if (!gpios) {
        return MX_ERR_INVALID_ARGS;
    }
    return pl061_proto_ops.read(gpios, pin, out_value);
}

static mx_status_t hi3660_gpio_write(void* ctx, unsigned pin, unsigned value) {
    hi3660_bus_t* bus = ctx;
    pl061_gpios_t* gpios = find_gpio(bus, pin);
    if (!gpios) {
        return MX_ERR_INVALID_ARGS;
    }
    return pl061_proto_ops.write(gpios, pin, value);
}

static mx_status_t hi3660_gpio_int_enable(void* ctx, unsigned pin, bool enable) {
    hi3660_bus_t* bus = ctx;
    pl061_gpios_t* gpios = find_gpio(bus, pin);
    if (!gpios) {
        return MX_ERR_INVALID_ARGS;
    }
    return pl061_proto_ops.int_enable(gpios, pin, enable);
}

static mx_status_t hi3660_gpio_get_int_status(void* ctx, unsigned pin, bool* out_status) {
    hi3660_bus_t* bus = ctx;
    pl061_gpios_t* gpios = find_gpio(bus, pin);
    if (!gpios) {
        return MX_ERR_INVALID_ARGS;
    }
    return pl061_proto_ops.get_int_status(gpios, pin, out_status);
}

static mx_status_t hi3660_gpio_int_clear(void* ctx, unsigned pin) {
    hi3660_bus_t* bus = ctx;
    pl061_gpios_t* gpios = find_gpio(bus, pin);
    if (!gpios) {
        return MX_ERR_INVALID_ARGS;
    }
    return pl061_proto_ops.int_clear(gpios, pin);
}

static gpio_protocol_ops_t gpio_ops = {
    .config = hi3660_gpio_config,
    .read = hi3660_gpio_read,
    .write = hi3660_gpio_write,
    .int_enable = hi3660_gpio_int_enable,
    .get_int_status = hi3660_gpio_get_int_status,
    .int_clear = hi3660_gpio_int_clear,
};

static mx_status_t hi3660_get_protocol(void* ctx, uint32_t proto_id, void* out) {
    if (proto_id == MX_PROTOCOL_GPIO) {
        gpio_protocol_t* proto = out;
        proto->ctx = ctx;
        proto->ops = &gpio_ops;
        return MX_OK;
    }
    return MX_ERR_NOT_SUPPORTED;
}

static mx_status_t hi3660_add_gpios(void* ctx, uint32_t start, uint32_t count, uint32_t mmio_index,
                                    const uint32_t* irqs, uint32_t irq_count) {
    hi3660_bus_t* bus = ctx;

    pl061_gpios_t* gpios = calloc(1, sizeof(pl061_gpios_t));
    if (!gpios) {
        return MX_ERR_NO_MEMORY;
    }

    mx_status_t status = pdev_map_mmio(&bus->pdev, mmio_index, MX_CACHE_POLICY_UNCACHED_DEVICE,
                                       (void *)&gpios->mmio_base, &gpios->mmio_size,
                                       &gpios->mmio_handle);
    if (status != MX_OK) {
        free(gpios);
        return status;
    }

    mtx_init(&gpios->lock, mtx_plain);
    gpios->irqs = irqs;
    gpios->irq_count = irq_count;
    list_add_tail(&bus->gpios, &gpios->node);

    return MX_OK;
}

static pbus_interface_ops_t hi3660_bus_ops = {
    .get_protocol = hi3660_get_protocol,
    .add_gpios = hi3660_add_gpios,
};

static void hi3660_release(void* ctx) {
    hi3660_bus_t* bus = ctx;
    pl061_gpios_t* gpios;

    while ((gpios = list_remove_head_type(&bus->gpios, pl061_gpios_t, node)) != NULL) {
        mx_vmar_unmap(mx_vmar_root_self(), (uintptr_t)gpios->mmio_base, gpios->mmio_size);
        mx_handle_close(gpios->mmio_handle);
        free(gpios);
    }

    free(bus);
}

static mx_protocol_device_t hi3660_device_protocol = {
    .version = DEVICE_OPS_VERSION,
    .release = hi3660_release,
};

static mx_status_t hi3660_bind(void* ctx, mx_device_t* parent, void** cookie) {
    platform_device_protocol_t pdev;
    if (device_get_protocol(parent, MX_PROTOCOL_PLATFORM_DEV, &pdev) != MX_OK) {
        return MX_ERR_NOT_SUPPORTED;
    }

    hi3660_bus_t* bus = calloc(1, sizeof(hi3660_bus_t));
    if (!bus) {
        return MX_ERR_NO_MEMORY;
    }

    list_initialize(&bus->gpios);
    memcpy(&bus->pdev, &pdev, sizeof(bus->pdev));

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "hi3660-bus",
        .ops = &hi3660_device_protocol,
        // nothing should bind to this device
        // all interaction will be done via the pbus_interface_t
        .flags = DEVICE_ADD_NON_BINDABLE,
    };

    mx_status_t status = device_add(parent, &args, NULL);
    if (status != MX_OK) {
        free(bus);
        return status;
    }

    pbus_interface_t intf;
    intf.ops = &hi3660_bus_ops;
    intf.ctx = bus;
    pdev_set_interface(&pdev, &intf);

    return MX_OK;
}

static mx_driver_ops_t hi3660_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = hi3660_bind,
};

MAGENTA_DRIVER_BEGIN(hi3660, hi3660_driver_ops, "magenta", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, MX_PROTOCOL_PLATFORM_DEV),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, 0x12D1),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, 0x0960),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_BUS_IMPLEMENTOR_DID),
MAGENTA_DRIVER_END(hi3660)
