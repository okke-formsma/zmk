
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

struct zmk_mouse_move_tick {
    struct vector2d speed;
    int64_t timestamp;
};

ZMK_EVENT_DECLARE(zmk_mouse_move_tick);

static inline struct zmk_mouse_move_tick_event *zmk_mouse_move_tick(int16_t x, int16_t y,
                                                                    int64_t timestamp) {
    struct vector2d speed = {x, y};
    return new_zmk_mouse_move_tick(
        (struct zmk_mouse_move_tick){.speed = speed, .timestamp = timestamp});
}
