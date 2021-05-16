/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/mouse_move_tick.h>
#include <zmk/endpoints.h>
#include <zmk/mouse/vector2d.h>

#include <sys/util.h>

// CLAMP will be provided by sys/util.h from zephyr 2.6 onward
#define CLAMP(x, min, max) MIN(MAX(x, min), max)

static int64_t previous_timestamp = 0;
static struct vector2d milli_location = {0};

static void clear_state() {
    previous_timestamp = 0;
    milli_location = (struct vector2d){0};
}

static int64_t ms_since_previous_tick(int64_t timestamp) {
    int64_t time_elapsed_ms;
    if (previous_timestamp == 0) {
        // First move after clear_state()
        time_elapsed_ms = 10; // todo: replace with configuration setting
    } else {
        time_elapsed_ms = timestamp - previous_timestamp;
    }
    previous_timestamp = timestamp;
    return time_elapsed_ms;
}

static struct vector2d move_constant(struct vector2d speed, int64_t time_elapsed_ms) {
    // We're keeping track of the current 'location' in millimoves. This helps to move accurately
    // when the target speed is not divisible by the tick frequency. For example, at 10ms (100hz)
    // and target speed 90, a naive implementation would move (int)(90/100)=0.
    struct vector2d new_milli_location = vector2d_move(milli_location, speed, time_elapsed_ms);
    struct vector2d move = vector2d_milli_difference(milli_location, new_milli_location);
    milli_location = new_milli_location;
    return move;
}

static void mouse_move_constant_tick(const struct zmk_mouse_move_tick *tick) {
    // Average out the target movement over a whole second
    // This allows for more precise speed control and account for timer deviation
    struct vector2d move;
    if (tick->speed.x == 0 && tick->speed.y == 0) {
        clear_state();
        move = (struct vector2d){0};
    } else {
        int64_t time_elapsed_ms = ms_since_previous_tick(tick->timestamp);
        move = move_constant(tick->speed, time_elapsed_ms);
    }
    zmk_hid_mouse_movement_set((int8_t)CLAMP(move.x, INT8_MIN, INT8_MAX),
                               (int8_t)CLAMP(move.y, INT8_MIN, INT8_MAX));
}

int constant_move_mouse_listener(const zmk_event_t *eh) {
    const struct zmk_mouse_move_tick *tick = as_zmk_mouse_move_tick(eh);
    if (tick) {
        mouse_move_constant_tick(tick);
        return 0;
    }
    return 0;
}

ZMK_LISTENER(constant_move_mouse_listener, constant_move_mouse_listener);
ZMK_SUBSCRIPTION(constant_move_mouse_listener, zmk_mouse_move_tick);