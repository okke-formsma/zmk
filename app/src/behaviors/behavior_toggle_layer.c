/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_toggle_layer

#include <device.h>
#include <drivers/behavior.h>
#include <logging/log.h>

#include <zmk/keymap.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct behavior_tog_config {};
struct behavior_tog_data {};

static int behavior_tog_init(const struct device *dev) { return 0; };

static int tog_keymap_binding_pressed(const struct behavior_state_changed *event) {
    LOG_DBG("position %d layer %d", event->position, event->param1);
    return zmk_keymap_layer_toggle(event->param1);
}

static int tog_keymap_binding_released(const struct behavior_state_changed *event) {
    LOG_DBG("position %d layer %d", event->position, event->param1);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_tog_driver_api = {
    .binding_pressed = tog_keymap_binding_pressed,
    .binding_released = tog_keymap_binding_released,
};

static const struct behavior_tog_config behavior_tog_config = {};

static struct behavior_tog_data behavior_tog_data;

DEVICE_AND_API_INIT(behavior_tog, DT_INST_LABEL(0), behavior_tog_init, &behavior_tog_data,
                    &behavior_tog_config, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                    &behavior_tog_driver_api);
