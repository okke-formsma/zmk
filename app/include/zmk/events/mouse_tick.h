
/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr.h>
#include <zmk/event_manager.h>
#include <zmk/mouse/vector2d.h>
#include <dt-bindings/zmk/mouse.h>

struct zmk_mouse_tick {
    struct vector2d max_move;
    struct vector2d max_scroll;
    int64_t timestamp;
};

ZMK_EVENT_DECLARE(zmk_mouse_tick);

static inline struct zmk_mouse_tick_event *zmk_mouse_tick(int16_t max_move_x, int16_t max_move_y,
                                                          int16_t max_scroll_x,
                                                          int16_t max_scroll_y, int64_t timestamp) {
    struct vector2d max_move = {max_move_x, max_move_y};
    struct vector2d max_scroll = {max_scroll_x, max_scroll_y};
    return new_zmk_mouse_tick((struct zmk_mouse_tick){
        .max_move = max_move,
        .max_scroll = max_scroll,
        .timestamp = timestamp,
    });
}
