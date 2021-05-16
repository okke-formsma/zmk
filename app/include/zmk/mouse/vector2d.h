/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr.h>

#pragma once

struct vector2d {
    int32_t x;
    int32_t y;
};

struct vector2d vector2d_move(struct vector2d location, struct vector2d speed, int64_t time);
struct vector2d vector2d_milli_difference(struct vector2d previous, struct vector2d new);