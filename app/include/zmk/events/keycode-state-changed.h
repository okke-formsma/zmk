/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr.h>
#include <dt-bindings/zmk/modifiers.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/event-manager.h>
#include <zmk/keys.h>

struct keycode_state_changed {
    struct zmk_event_header header;
    u8_t usage_page;
    u32_t keycode;
    u8_t implicit_modifiers;
    u8_t explicit_modifiers;
    bool state;
    s64_t timestamp;
};

ZMK_EVENT_DECLARE(keycode_state_changed);

static inline struct keycode_state_changed *
keycode_state_changed_from_encoded(u32_t encoded, bool pressed, s64_t timestamp) {
    u16_t page = HID_USAGE_PAGE(encoded) & 0xFF;
    u16_t id = HID_USAGE_ID(encoded);

    if (!page) {
        page = HID_USAGE_KEY;
    }

    struct keycode_state_changed *ev = new_keycode_state_changed();
    ev->usage_page = page;
    ev->keycode = id;
    ev->state = pressed;
    ev->timestamp = timestamp;

    if (IS_MOD(page, id)) {
        ev->explicit_modifiers = SELECT_MODS(encoded);
    } else {
        ev->explicit_modifiers = SELECT_MODS(encoded);
    }

    return ev;
}
