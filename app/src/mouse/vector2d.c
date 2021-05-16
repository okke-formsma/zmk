/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr.h>
#include <zmk/mouse/vector2d.h>

struct vector2d vector2d_move(struct vector2d milli_location, struct vector2d speed, int64_t time) {
    return (struct vector2d){
        .x = milli_location.x + speed.x * time,
        .y = milli_location.y + speed.y * time,
    };
}

struct vector2d vector2d_milli_difference(struct vector2d previous, struct vector2d new) {
    // Do the division before the subtraction to prevent accumulation of rounding errors.
    return (struct vector2d){
        .x = new.x / 1000 - previous.x / 1000,
        .y = new.y / 1000 - previous.y / 1000,
    };
}