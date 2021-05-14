
/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr.h>
#include <zmk/event_manager.h>
#include <dt-bindings/zmk/mouse.h>

struct zmk_mouse_scroll_state_changed {
    int8_t x;
    int8_t y;
    bool state;
    int64_t timestamp;
};

ZMK_EVENT_DECLARE(zmk_mouse_scroll_state_changed);

static inline struct zmk_mouse_scroll_state_changed_event *
zmk_mouse_scroll_state_changed_from_encoded(uint32_t encoded, bool pressed, int64_t timestamp) {
    int8_t x = SCROLL_HOR_DECODE(encoded);
    int8_t y = SCROLL_VERT_DECODE(encoded);

    return new_zmk_mouse_scroll_state_changed((struct zmk_mouse_scroll_state_changed){
        .x = x, .y = y, .state = pressed, .timestamp = timestamp});
}
