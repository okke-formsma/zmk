/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_combo

#include <device.h>
#include <drivers/behavior.h>
#include <logging/log.h>
#include <sys/dlist.h>

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
#define ZMK_BHV_COMBO_MAX_POSITIONS 100

#define ZMK_BHV_COMBO_MAX_COMBOS 20
#define ZMK_BHV_COMBO_MAX_COMBOS_PER_KEY 5
#define ZMK_BHV_COMBO_MAX_PRESSED_COMBOS 20
#define ZMK_BHV_COMBO_MAX_KEYS_PER_COMBO 4

#define ZMK_BHV_COMBO_POSITION_NOT_USED -1

typedef const struct behavior_combo_config {
    s32_t timeout_ms;
    // todo: this could be a linked list too?
    s32_t key_positions[ZMK_BHV_COMBO_MAX_KEYS_PER_COMBO]; 
    s32_t key_position_len;
    struct zmk_behavior_binding behavior;
    // key_positions_pressed is filled with key_positions when the combo is pressed.
    // The keys are removed from this array when they are released.
    // Once this array is empty, the behavior is released.
    s32_t key_positions_pressed[ZMK_BHV_COMBO_MAX_KEYS_PER_COMBO];
} combo;

static struct behavior_combo_data behavior_combo_data;

typedef struct combo_dlist_item {
    sys_dnode_t dnode;
    combo *combo;
} combo_dlist_item;


/* combos maps key positions to a linked list of combo_dlist_items */
sys_dlist_t combos_per_key[ZMK_BHV_COMBO_MAX_POSITIONS];

/* candidates contains the combo_dlist_items that fit the currently pressed keys */
sys_dlist_t candidates;

/* pressed_combos contains the combos that are currently pressed.
When all key_positions_pressed in the pressed combo are released, the combo is released. */
sys_dlist_t pressed_combos;

/* pressed_keys contains position_event_dlist_items that are captured until a combo is complete */
typedef struct position_event_dlist_item {
    sys_dnode_t dnode;
    struct position_state_changed *event;
} position_event_dlist_item;

sys_dlist_t pressed_keys;

/* timers */
s32_t release_at;
struct k_delayed_work release_after_timer;

/* insertion sort */
static void sort(s32_t *array, int length) {
    s32_t swap;
    for (i=1; i < length; i++) {
        for (j=i; j>0; j--) {
            if(array[j-1] <= a[j]) {
                break;
            }
            swap = array[j];
            array[j] = array[j-1];
            array[j-1] = swap;
        }
    }
}

/* initialize the combos_per_key array */
static void initialize_combos_per_key() {
    for (int position = 0; position < ZMK_BHV_COMBO_MAX_POSITIONS; position++) {
        sys_dlist_init(&combos_per_key[position]);
    }
}

/* store the combo key pointer in the combos array, one pointer for each key position */
static int initialize_combo(combo *combo) {
    for (int i = 0; i < combo->key_position_len; i++) {
        s32_t position = combo->key_positions[i];
        combo_dlist_item *item = (combo_dlist_item *)k_malloc(sizeof(combo_dlist_item));
        sys_dlist_append(&combos_per_key[position], &item->dnode);
    }
    return 0;
}

/* add an event to the pressed_keys array */
static int capture_pressed_key(struct position_state_changed *ev) {
    position_event_dlist_item *item = (position_event_dlist_item *)k_malloc(sizeof(position_event_dlist_item));
    item->event = ev;
    sys_dlist_append(&pressed_keys, &item->dnode);
    return ZMK_EV_EVENT_CAPTURED;
}

/* pressed_keys are released when they are not part of a combo */
static void release_pressed_keys() {
    position_event_dlist_item *item;
    position_event_dlist_item *item_safe;
    SYS_DLIST_FOR_EACH_CONTAINER_SAFE(&pressed_keys, item, item_safe, dnode) {
        ZMK_EVENT_RELEASE(item->event);
        sys_dlist_remove(&item->dnode);
        k_free(item);
    }
}

/* pressed_keys events are freed when they are used by a combo */
static void free_pressed_keys() {
    position_event_dlist_item *item;
    position_event_dlist_item *item_safe;
    SYS_DLIST_FOR_EACH_CONTAINER_SAFE(&pressed_keys, item, item_safe, dnode) {
        k_free(item->event);
        sys_dlist_remove(&item->dnode);
        k_free(item);
    }
}

/* initialize the list of candidates */
static int setup_candidates_for_first_keypress(s32_t position) {
    combo_dlist_item *item;
    int i;
    SYS_DLIST_FOR_EACH_CONTAINER(&combos_per_key[position], item, dnode) {
        combo_dlist_item *candidate = (combo_dlist_item *)k_malloc(sizeof(combo_dlist_item));
        candidate->combo = item->combo;
        sys_dlist_append(&candidates, &item->dnode);
        i++;
    }
    return i;
}

/* clear the set of candidates */
static void clear_candidates() {
    combo_dlist_item *item;
    combo_dlist_item *item_safe;
    SYS_DLIST_FOR_EACH_CONTAINER_SAFE(&candidates, item, item_safe, dnode) {
        sys_dlist_remove(&candidates);
        k_free(&item->dnode);
    }
}

/* filter(candidates, lambda c: c in combos_per_key[position]) */
static int filter_candidates(s32_t position) {
    int matches = 0;
    combo_dlist_item *candidate;
    combo_dlist_item *item_safe;
    SYS_DLIST_FOR_EACH_CONTAINER_SAFE(&candidates, candidate, item_safe, dnode) {
        bool found = false;
        combo_dlist_item *combo_item;
        SYS_DLIST_FOR_EACH_CONTAINER(&combos_per_key[position], combo_item, dnode) {
            if (candidate->combo == combo_item->combo) {
                found = true;
                break;
            }
        }
        if (found) {
            matches++;
        } else {
            sys_dlist_remove(&candidate->dnode);
        }
    }
    return matches;
}

/* return set(combo->config->key_positions) == set(pressed_keys) */
static bool is_completely_pressed(combo *candidate) {
    // this code assumes set(pressed_keys) <= set(candidate->config->key_positions)
    // this invariant is enforced by filter_candidates
    position_event_dlist_item *event_item;
    SYS_DLIST_FOR_EACH_CONTAINER(&pressed_keys, event_item, dnode) {
        bool found = false;
        for (int i = 0; i<ZMK_BHV_COMBO_MAX_KEYS_PER_COMBO; i++) {
            if (event_item->event->position == candidate->key_positions[i]) {
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

/**
 * returns the combo which set of pressed_keys is identical to the pressed_keys.
 * If no such combo exists, return NULL.
 */
combo *currently_pressed_combo() {
    combo_dlist_item *candidate;
    SYS_DLIST_FOR_EACH_CONTAINER(&candidates, candidate, dnode) {
        if (is_completely_pressed(candidate->combo)) {
            return candidate->combo;
        }
    }
    return NULL;
}

/**
 * store the combo in the pressed_combos array and fill it's key_positions_pressed array
 */
static inline int store_pressed_combo(combo *combo) {
    for (int i = 0; i<ZMK_BHV_COMBO_MAX_KEYS_PER_COMBO; i++) {
        combo->key_positions_pressed[i] = combo->key_positions[i];
    }
    
}

/**
 * returns true if the combo has no currently pressed keys
 */
static inline bool is_pressed_combo_released(combo *combo) {
    for (i = 0; i<ZMK_BHV_COMBO_MAX_KEYS_PER_COMBO; i++) {
        if (combo->key_positions_pressed[i] != ZMK_BHV_COMBO_POSITION_NOT_USED) {
            return false;
        }
    }
    return true;
}

/**
 * go through all currently pressed combos_per_key and see if the released key
 * is part of the combo. If all keys of a combo are released, release
 * the entire combo.
 */
static inline void release_combo_key(s32_t position) {
    for (i = 0; i<ZMK_BHV_COMBO_MAX_PRESSED_COMBOS; i++) {
        combo *combo = pressed_combos[i]
        if (combo == NULL) {
            continue;
        }
        for (j = 0; j<ZMK_BHV_COMBO_MAX_KEYS_PER_COMBO; j++) {
            if (combo->key_positions_pressed[j] != position) {
                continue;
            }
            combo->key_positions_pressed[j] = ZMK_BHV_COMBO_POSITION_NOT_USED;
            if (is_pressed_combo_released(combo)) {
                release_combo_behavior(combo);
                pressed_combos[i] = NULL;
                return;
            }
        }
    }
}

static inline int press_combo_behavior(combo *combo, s32_t timestamp) {
    const struct zmk_behavior_binding *behavior = &combo->behavior;
    struct device *behavior_device = device_get_binding(behavior->behavior_dev);
    return behavior_keymap_binding_pressed(behavior_device, combo->key_positions[0], behavior->param1, behavior->param2, timestamp);
}

static inline int release_combo_behavior(combo *combo, s32_t timestamp) {
    const struct zmk_behavior_binding *behavior = &combo->behavior;
    struct device *behavior_device = device_get_binding(behavior->behavior_dev);
    return behavior_keymap_binding_released(behavior_device, combo->key_positions[0], behavior->param1, behavior->param2, timestamp);
}


// A combo is never pressed in a keymap.
static int on_combo_binding_pressed(struct device *dev, u32_t position, u32_t _, u32_t __,
                                    s64_t timestamp) {
    return 0;
}

static int on_combo_binding_released(struct device *dev, u32_t position, u32_t _, u32_t __,
                                     s64_t timestamp) {
    return 0;
}

static const struct behavior_driver_api behavior_combo_driver_api = {
    .binding_pressed = on_combo_binding_pressed,
    .binding_released = on_combo_binding_released,
};

static int combo_keycode_state_down(struct position_state_changed *ev) {
    if (pressed_keys[0] == NULL) {
        int num_candidates = setup_candidates_for_first_keypress(ev->position);
        if(num_candidates > 0) {
            return capture_pressed_key(ev);
        }
        return 0;
    }

    combo *previous_combo = currently_pressed_combo();
    int num_candidates = filter_candidates(ev->position);
    switch (num_candidates) {
        case 0:
            if (previous_combo != NULL) {
                k_delayed_work_cancel(&release_after_timer);
                press_combo_behavior(previous_combo, ev->timestamp);
                if(store_pressed_combo(combo) == -ENOMEM) {
                    return 0;
                }
                return ZMK_EV_EVENT_HANDLED;
            } else {
                release_pressed_keys();
                return 0;
            }
        case 1:
            combo *current_combo = currently_pressed_combo();
            if (current_combo != NULL) {
                k_delayed_work_cancel(&release_after_timer);
                press_combo_behavior(currently_pressed_combo(), ev->timestamp);
                return ZMK_EV_EVENT_HANDLED;
            } else {
                return capture_pressed_key(ev);
            }
        default:
            return capture_pressed_key(ev);
    }
}

static int combo_keycode_state_up(struct position_state_changed *ev) {
    k_delayed_work_cancel(&release_after_timer);
    combo* current_combo = currently_pressed_combo();
    if (current_combo == NULL) {
        release_pressed_keys();
    } else {
        press_combo_behavior(currently_pressed_combo(), ev->timestamp);
    }
    release_combo_key(ev->position);
    return 0;
}

static int combo_keycode_state_changed_listener(const struct zmk_event_header *eh) {
    if (!is_position_state_changed(eh)) {
        return 0;
    }

    struct position_state_changed *ev = cast_position_state_changed(eh);
    if(ev->state) { //keydown
        return combo_keycode_state_down(ev);
    } else { //keyup
        return combo_keycode_state_up(ev);
    }
}

ZMK_LISTENER(behavior_combo, combo_keycode_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_combo, keycode_state_changed);

void behavior_combo_timer_handler(struct k_work *item) {
    release_pressed_keys();
}

static int behavior_combo_init(struct device *dev) {
    static bool init_first_run = true;
    if (init_first_run) {
        initialize_combos_per_key()
        k_delayed_work_init(&release_after_timer, behavior_combo_timer_handler);
        sys_dlist_init(&pressed_keys)
        sys_dlist_init(&pressed_combos)
        sys_dlist_init(&candidates)
        init_first_run = false;
    }

    return initialize_combo(dev->config_info);
}

#define _TRANSFORM_ENTRY(idx, node)                                                                \
    {                                                                                              \
        .behavior_dev = DT_LABEL(DT_INST_PHANDLE_BY_IDX(node, bindings, idx)),                     \
        .param1 = COND_CODE_0(DT_INST_PHA_HAS_CELL_AT_IDX(node, bindings, idx, param1), (0),       \
                              (DT_INST_PHA_BY_IDX(node, bindings, idx, param1))),                  \
        .param2 = COND_CODE_0(DT_INST_PHA_HAS_CELL_AT_IDX(node, bindings, idx, param2), (0),       \
                              (DT_INST_PHA_BY_IDX(node, bindings, idx, param2))),                  \
    }

#define KP_INST(n)                                                                                 \
    static struct behavior_combo_config behavior_combo_config_##n = {                              \
        .timeout_ms = DT_INST_PROP(n, timeout_ms),   \
        .key_positions = DT_INST_PROP(n, key_positions),   \
        .key_position_len = DT_INST_PROP_LEN(n, key_positions),   \
        .behavior = _TRANSFORM_ENTRY(0, n),  \
    };                                                                                             \
    DEVICE_AND_API_INIT(behavior_combo_##n, DT_INST_LABEL(n), behavior_combo_init,                 \
                        &behavior_combo_data, &behavior_combo_config_##n, APPLICATION,             \
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_combo_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)

#endif