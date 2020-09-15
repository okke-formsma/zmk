/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_chord

#include <device.h>
#include <drivers/behavior.h>
#include <logging/log.h>
#include <zmk/behavior.h>

#include <zmk/matrix.h>
#include <zmk/endpoints.h>
#include <zmk/event-manager.h>
#include <zmk/events/position-state-changed.h>
#include <zmk/events/keycode-state-changed.h>
#include <zmk/events/modifiers-state-changed.h>
#include <zmk/hid.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_NODE_EXISTS(DT_DRV_INST(0))

// max_keys could be set to the actual number of keys on the board
#define ZMK_BHV_CHORD_MAX_KEYS 100
#define ZMK_BHV_CHORD_MAX_CHORDS 20
#define ZMK_BHV_CHORD_MAX_CHORDS_PER_KEY 5
#define ZMK_BHV_CHORD_MAX_KEYS_PER_CHORD 4

#define ZMK_BHV_CHORD_POSITION_NOT_USED -1

struct behavior_chord_config {
    s32_t tapping_term_ms;
    s32_t key_positions[ZMK_BHV_CHORD_MAX_KEYS_PER_CHORD];
    struct zmk_behavior_binding behavior;
};

// todo: maybe we don't need the 'chord struct'
struct chord {
    const struct behavior_chord_config *config;
};

// todo put this in behavior_chord_data
s32_t release_at;
struct k_delayed_work release_after_timer;
s32_t pressed_keys[ZMK_BHV_CHORD_MAX_CHORDS_PER_KEY];
struct chord *candidates[ZMK_BHV_CHORD_MAX_CHORDS_PER_KEY];
struct chord *chords[ZMK_BHV_CHORD_MAX_KEYS][ZMK_BHV_CHORD_MAX_CHORDS_PER_KEY];

struct behavior_chord_data {
} static struct behavior_chord_data behavior_chord_data;

static void initialize_chords() {
    for (int i = 0; i < ZMK_BHV_CHORD_MAX_KEYS; i++) {
        for (int j = 0; j < ZMK_BHV_CHORD_MAX_CHORDS_PER_KEY; j++) {
            chords[i][j] = NULL;
        }
    }
}

/* store the chord key pointer in the chords array, one pointer for each key position */
static int initialize_chord(struct chord *chord) {
    for (int i = 0; i < ZMK_BHV_CHORD_MAX_KEYS_PER_CHORD; i++) {
        s32_t position = chord->config->key_position[i];
        for (int j = 0; j < ZMK_BHV_CHORD_MAX_CHORDS_PER_KEY; j++) {
            if (chords[position][j] == NULL) {
                chords[position][j] = chord;
                break;
            }
        }
        LOG_ERR("Too many chords for key position %d, max %d.", position,
                ZMK_BHV_CHORD_MAX_CHORDS_PER_KEY)
        return -ENOMEM
    }
    return 0;
}

static int add_pressed_key(s32_t position) {
    for (int i = 0; i < ZMK_BHV_CHORD_MAX_CHORDS_PER_KEY; i++) {
        if (pressed_keys[i] == ZMK_BHV_CHORD_POSITION_NOT_USED) {
            pressed_keys[i] = position;
            return 0;
        }
    }
    LOG_ERR("Too many pressed chord keys.")
    return -ENOMEM;
}

static void clear_pressed_keys() {
    for (int i = 0; i < ZMK_BHV_CHORD_MAX_CHORDS_PER_KEY; i++) {
        pressed_keys[i] = ZMK_BHV_CHORD_POSITION_NOT_USED;
    }
}

static void setup_candidates_for_first_keypress(s32_t position) {
    for (int i = 0; i < ZMK_BHV_CHORD_MAX_CHORDS_PER_KEY; i++) {
        candidates[i] = chords[position][i];
    }
}

static int clear_candidates() {
    for (int i = 0; i < ZMK_BHV_CHORD_MAX_CHORDS_PER_KEY; i++) {
        if (candidates[i] == NULL) {
            return;
        }
        candidates[i] = NULL;
    }
}

/* filter(candidates, lambda c: c in chords[position]) */
static int filter_candidates(s32_t position) {
    int matches = 0;
    for (int candidate_idx = 0; candidate_idx < ZMK_BHV_CHORD_MAX_CHORDS_PER_KEY; candidate_idx++) {
        if (candidates[candidate_idx] == NULL) {
            break;
        }
        for (int chord_idx = 0; chord_idx < ZMK_BHV_CHORD_MAX_CHORDS_PER_KEY; chord_idx++) {
            if (chords[chord_idx] == NULL) {
                candidates[candidate_idx] = NULL;
                break;
            }
            if (candidates[candidate_idx] == chords[position][chord_idx]) {
                if (candidate_idx != matches) {
                    candidates[matches] = candidates[candidate_idx];
                    candidates[candidate_idx] = NULL;
                }
                matches++;
                break;
            }
        }
    }
    return matches;
}

/* set(chord->config->key_positions) == set(pressed_keys) */
static bool is_completely_pressed(struct chord *candidate) {
    // this code assumes set(pressed_keys) <= set(candidate->config->key_positions)
    // this invariant is enforced by filter_candidates
    for (int candidate_key_idx = 0; candidate_key_idx < ZMK_BHV_CHORD_MAX_KEYS_PER_CHORD;
         candidate_key_idx++) {
        if (candidate->config->key_positions[candidate_key_idx] == NULL) {
            break;
        }
        bool found = false;
        for (int pressed_key_idx = 0; pressed_key_idx < ZMK_BHV_CHORD_MAX_KEYS_PER_CHORD;
             pressed_key_idx++) {
            if (pressed_keys[pressed_key_idx] == NULL) {
                break;
            }
            if (pressed_keys[pressed_key_idx] ==
                candidate->config->key_positions[candidate_key_idx]) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

static struct chord *currently_pressed_chord() {
    for (int i = 0; i < ZMK_BHV_CHORD_MAX_KEYS_PER_CHORD; i++) {
        if (candidate[i] == NULL) {
            return NULL;
        }
        if (is_completely_pressed(candidate[i])) {
            return candidate[i];
        }
    }
    return NULL;
}

// todo: this should be a bound thingy
static inline int press_chord_behavior(struct active_chord *chord, s32_t timestamp) {
    const struct zmk_behavior_binding *behavior = &chord->config->behavior;
    struct device *behavior_device = device_get_binding(behavior->behavior_dev);
    return behavior_keymap_binding_pressed(behavior_device, chord->position, chord->param1,
                                           chord->param2, timestamp);
}

static inline int release_chord_behavior(struct active_chord *chord, s32_t timestamp) {
    const struct zmk_behavior_binding *behavior = &chord->config->behavior;
    struct device *behavior_device = device_get_binding(behavior->behavior_dev);
    return behavior_keymap_binding_released(behavior_device, chord->position, chord->param1,
                                            chord->param2, timestamp);
}

static int stop_timer(struct chord *chord) {
    int timer_cancel_result = k_delayed_work_cancel(&chord->release_after_timer);
    if (timer_cancel_result == -EINPROGRESS) {
        // too late to cancel, we'll let the timer handler clear up.
        chord->timer_is_cancelled = true;
    }
    return timer_cancel_result;
}

static int on_chord_binding_pressed(struct device *dev, u32_t position, u32_t _, u32_t __,
                                    s64_t timestamp) {
    return 0;
}

static int on_chord_binding_released(struct device *dev, u32_t position, u32_t _, u32_t __,
                                     s64_t timestamp) {
    return 0;
}

static const struct behavior_driver_api behavior_chord_driver_api = {
    .binding_pressed = on_chord_binding_pressed,
    .binding_released = on_chord_binding_released,
};

static int chord_keycode_state_changed_listener(const struct zmk_event_header *eh) {
    if (!is_keycode_state_changed(eh)) {
        return 0;
    }

    // No other key was pressed. Start the timer.
    chord->release_at = timestamp + chord->config->release_after_ms;
    // adjust timer in case this behavior was queued by a hold-tap
    s32_t ms_left = chord->release_at - k_uptime_get();
    if (ms_left > 0) {
        k_delayed_work_submit(&chord->release_after_timer, K_MSEC(ms_left));
    }

    struct keycode_state_changed *ev = cast_keycode_state_changed(eh);
    for (int i = 0; i < ZMK_BHV_CHORD_MAX_HELD; i++) {
        struct active_chord *chord = &active_chords[i];
        if (chord->position == ZMK_BHV_CHORD_POSITION_NOT_USED || chord->position == ev->position) {
            continue;
        }
        // If events were queued, the timer event may be queued late or not at all.
        // Release the one-shot if the timer should've run out in the meantime.
        if (chord->release_at != 0 && ev->timestamp > chord->release_at) {
            release_chord_behavior(chord, chord->release_at);
            if (stop_timer(chord)) {
                clear_chord(chord);
            }
            continue;
        }

        if (ev->state) { // key down
            if (chord->modified_key_position != ZMK_BHV_CHORD_POSITION_NOT_USED) {
                continue;
            }
            chord->modified_key_position = ev->position;
            if (chord->release_at) {
                stop_timer(chord);
            }
        } else { // key up
            if (chord->modified_key_position != ev->position || chord->release_at == 0) {
                continue;
            }
            release_chord_behavior(chord, ev->timestamp);
        }
    }
    return 0;
}

ZMK_LISTENER(behavior_chord, chord_keycode_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_chord, keycode_state_changed);

void behavior_chord_timer_handler(struct k_work *item) {
    struct active_chord *chord = CONTAINER_OF(item, struct active_chord, release_after_timer);
    if (chord->position == ZMK_BHV_CHORD_POSITION_NOT_USED) {
        return;
    }
    if (!chord->timer_is_cancelled) {
        release_chord_behavior(chord, k_uptime_get());
    }
    clear_chord(chord);
}

static int behavior_chord_init(struct device *dev) {
    static bool init_first_run = true;
    if (init_first_run) {
        initialize_chords();
        k_delayed_work_init(&active_chords[i].release_after_timer, behavior_chord_timer_handler);
        init_first_run = false;
    }

    initialize_chord(dev->config);
    return 0;
}

#define _TRANSFORM_ENTRY(idx, node)                                                                \
    {                                                                                              \
        .behavior_dev = DT_LABEL(DT_INST_PHANDLE_BY_IDX(node, bindings, idx)),                     \
        .param1 = COND_CODE_0(DT_INST_PHA_HAS_CELL_AT_IDX(node, bindings, idx, param1), (0),       \
                              (DT_INST_PHA_BY_IDX(node, bindings, idx, param1))),                  \
        .param2 = COND_CODE_0(DT_INST_PHA_HAS_CELL_AT_IDX(node, bindings, idx, param2), (0),       \
                              (DT_INST_PHA_BY_IDX(node, bindings, idx, param2))),                  \
    },

#define KP_INST(n)                                                                                 \
    static struct behavior_chord_config behavior_chord_config_##n = {                              \
        .behavior = _TRANSFORM_ENTRY(0, n).release_after_ms = DT_INST_PROP(n, release_after_ms),   \
    };                                                                                             \
    DEVICE_AND_API_INIT(behavior_chord_##n, DT_INST_LABEL(n), behavior_chord_init,                 \
                        &behavior_chord_data, &behavior_chord_config_##n, APPLICATION,             \
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_chord_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)

#endif