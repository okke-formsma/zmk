/*
 * Copyright (c) 2020 Cody McGinnis
 *
 * SPDX-License-Identifier: MIT
 */

#include <device.h>
#include <drivers/behavior.h>
#include <logging/log.h>
#include <zmk/behavior.h>

#include <zmk/matrix.h>
#include <zmk/endpoints.h>
#include <zmk/event-manager.h>
#include <zmk/events/position-state-changed.h>
#include <zmk/hid.h>

#define DT_DRV_COMPAT zmk_behavior_tap_hold

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define ZMK_BHV_TAP_HOLD_MAX_HELD 10
#define ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC 40

#define TH_TYPE_MOD_PREFERRED 0
#define TH_TYPE_BALANCED 1
#define TH_TYPE_TAP_PREFERRED 2

// todo: this should be done in config
#define CURRENT_TYPE TH_TYPE_BALANCED

#if DT_NODE_EXISTS(DT_DRV_INST(0))
struct behavior_tap_hold_behaviors {
  struct zmk_behavior_binding tap;
  struct zmk_behavior_binding hold;
};

// this data is specific for each keypress
struct active_tap_hold {
  u32_t position;
  bool is_decided;
  bool is_hold;
};

typedef k_timeout_t (*timer_func)();

// this data is shared between all tap-holds, no matter which config.
struct behavior_tap_hold_shared_data {
  int first_active_tap_hold;
  struct active_tap_hold active_tap_holds[ZMK_BHV_TAP_HOLD_MAX_HELD];
  int first_captured_position_event;
  struct position_state_changed* captured_position_events[ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC];
};

// this data is specific to a configured behavior
struct behavior_tap_hold_data {
  //todo: add timestamp when keydown was seen
  struct k_timer timer;
  struct behavior_tap_hold_shared_data* shared_data;
};

struct behavior_tap_hold_config {
  timer_func hold_ms;
  struct behavior_tap_hold_behaviors* behaviors;
};

void capture_position_event(struct behavior_tap_hold_data *data, struct position_state_changed* event) {
  for (int i = data->shared_data->first_captured_position_event; i < ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC; i++) {
    struct position_state_changed* captured_position = &data->shared_data->captured_position_events[i];
    if (captured_position->event == NULL) {
      captured_position->event = event;
    }
  }
  return -ENOMEM;
}


struct position_state_changed* find_pending_position(struct behavior_tap_hold_data *data, u32_t position)
{
  //loop backwards so we find the correct pending event if a position was pressed multiple times
  for (int i = ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC-1; i >= data->shared_data->first_captured_position_event; i--) {
    struct position_state_changed* captured_position = &data->shared_data->captured_position_events[i];
    if (captured_position->event == NULL) {
      continue;
    }

    if (captured_position->event->position == position) {
      return captured_position;
    }
  }

  return NULL;
}


/* returns the current undecided tap_hold key or NULL if none are active. */
struct active_tap_hold* undecided_tap_hold(struct behavior_tap_hold_data *data) {
  for (int i = data->shared_data->first_active_tap_hold; i < ZMK_BHV_TAP_HOLD_MAX_HELD; i++) {
    struct active_tap_hold* active_tap_hold = &data->shared_data->active_tap_holds[i];
    if(active_tap_hold->position == -1) {
      continue;
    }
    if (!active_tap_hold->is_decided) {
      LOG_DBG("Found pending tap-hold on position %d", active_tap_hold->position);
      return active_tap_hold;
    }
  }
  return NULL;
}

void init_active_tap_holds(struct behavior_tap_hold_data *data) {
  data->shared_data->first_active_tap_hold = 0;
  for (int i = 0; i < ZMK_BHV_TAP_HOLD_MAX_HELD; i++) {
    struct active_tap_hold* active_tap_hold = &data->shared_data->active_tap_holds[i];
    active_tap_hold->position = -1;
    active_tap_hold->is_decided = 0;
  }
}

void init_captured_position_events(struct behavior_tap_hold_data *data) {
  data->shared_data->first_captured_position_event = 0;
  for (int i = 0; i < ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC; i++) {
    struct position_state_changed* captured_position = &data->shared_data->captured_position_events[i];
    captured_position->event = NULL;
  }
}

struct active_tap_hold* find_tap_hold(struct behavior_tap_hold_data *data, u32_t position) {
  for (int i = data->shared_data->first_active_tap_hold; i < ZMK_BHV_TAP_HOLD_MAX_HELD; i++) {
    struct active_tap_hold* active_tap_hold = &data->shared_data->active_tap_holds[i];
    if (active_tap_hold->position == position) {
      return active_tap_hold;
    }
  }
  return NULL;
}


void release_captured_positions(struct behavior_tap_hold_data *data) {
  struct behavior_tap_hold_shared_data *shared = data->shared_data;
  
  // copy the events queue on the stack, as the next active mod-tap reuses the same global array.
  // alternative is to keep an array on the heap for each behavior instance or a linked-list.
  struct position_state_changed* events_queue[ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC];
  memcpy(events_queue, data->shared_data->captured_position_events, sizeof(position_state_changed[ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC]));
  int next_event = data->shared_data->first_captured_position_event;
  init_captured_position_events();
  
  for(; next_event < ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC; next_event++) {
    struct position_state_changed* captured_position = events_queue[next_event];
    if(captured_position->event = NULL) {
      return;
    }
    LOG_DBG("Releasing key press for position 0x%02X state %s", ev->position, (ev->state ? "pressed" : "released"));
    ZMK_EVENT_RAISE(ev);
    k_msleep(10);
  }
}


struct position_state_changed* find_captured_position_event(struct behavior_tap_hold_data *data, u32_t position)
{
  // loop in reverse order so the last keydown event is found if events
  // are queued for the same position.
  for (int i = ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC-1; i >= 0; i--) {
    struct position_state_changed* next_captured_event = &data->shared_data->captured_position_events[i];
    if (next_captured_event->event == NULL) {
      continue;
    }

    if (next_captured_event->event->position == position) {
      return next_captured_event;
    }
  }

  return NULL;
}

static void timer_handler(struct k_timer *timer)
{
  const struct behavior_tap_hold_config *cfg = k_timer_user_data_get(timer);
  const struct behavior_tap_hold_behaviors *behaviors = cfg->behaviors;
  LOG_DBG("timer %p up: hold binding name: %s", timer, log_strdup(behaviors->hold.behavior_dev));
  struct device *behavior = device_get_binding(behaviors->hold.behavior_dev);
  if (behavior) {
    behavior_keymap_binding_pressed(behavior, 0, behaviors->hold.param1, behaviors->hold.param2);
  }
}

static int behavior_tap_hold_init(struct device *dev)
{
  struct behavior_tap_hold_data *data = dev->driver_data;
  init_tap_holds(data);

  k_timer_init(&data->timer, timer_handler, NULL);
  k_timer_user_data_set(&data->timer, (void*)dev->config_info);

  return 0;
}

static int on_keymap_binding_pressed(struct device *dev, u32_t position, u32_t _, u32_t __)
{
  struct behavior_tap_hold_data *data = dev->driver_data;
  const struct behavior_tap_hold_config *cfg = dev->config_info;


  LOG_DBG("key down: tap-hold on position: %d", position);
  LOG_DBG("timer %p started", &data->timer);
  k_timer_start(&data->timer, cfg->hold_ms(), K_NO_WAIT);

  return 0;
}

static int on_keymap_binding_released(struct device *dev, u32_t position, u32_t _, u32_t __)
{
  struct behavior_tap_hold_data *data = dev->driver_data;
  const struct behavior_tap_hold_config *cfg = dev->config_info;

  //uint32_t ticks_left = k_timer_remaining_ticks(&data->timer);
  k_timer_stop(&data->timer);

  struct active_tap_hold * tap_hold = find_tap_hold(data, position);
  if (!tap_hold->is_decided) {
    LOG_DBG("key up: tap-hold on position: %d", position);
    tap_hold->is_decided = 1;
    tap_hold->is_hold = 0;
  }

  //todo: release keys

  struct zmk_behavior_binding *behavior;
  if (tap_hold->is_hold) {
    behavior = &cfg->behaviors->hold;
  } else {
    behavior = &cfg->behaviors->tap;
  }
  struct device *behavior_device = device_get_binding(behavior->behavior_dev);
  return behavior_keymap_binding_released(behavior_device, position, behavior->param1, behavior->param2);
}


int behavior_tap_hold_listener(const struct zmk_event_header *eh)
{
  struct device *dev = device_get_binding(DT_INST_LABEL(0));
  struct behavior_tap_hold_data *data = dev->driver_data;
  struct active_tap_hold *tap_hold = undecided_tap_hold(data);
  if (!is_position_state_changed(eh) || tap_hold == NULL) {
    return 0;
  }
  struct position_state_changed *ev = cast_position_state_changed(eh);

  evaluate_event_for_tap_hold(tap_hold, ev);

  capture_position_event(data, ev);
  return ZMK_EV_EVENT_CAPTURED;
}

void evaluate_event_for_tap_hold(active_tap_hold *tap_hold, position_state_changed *ev){
  struct position_state_changed* pending_event;
  if (ev->state) { // key down
    LOG_DBG("Pending tap-hold. Capturing position %d down event", ev->position);
    if(!tap_hold->is_decided) {
      //only for mod-preferred behavior
    }
  } else if((pending_event = find_captured_position_event(data, ev->position)) != NULL) {
    LOG_DBG("Pending tap-hold. Capturing position %d up event", ev->position);
    if(!tap_hold->is_decided) {
      tap_hold->is_decided = 1;
      tap_hold->is_hold = 1;
    }
    // clear tap-hold
  } else {
  //todo: allow key-up events for non-mod keys pressed before the TH was pressed.
  // see scenario 3c/3d vs 3a/3b.
  }
}



static const struct behavior_driver_api behavior_tap_hold_driver_api = {
  .binding_pressed = on_keymap_binding_pressed,
  .binding_released = on_keymap_binding_released,
};

struct behavior_tap_hold_shared_data behavior_tap_hold_shared_data_instance = {};
#endif

#define _TRANSFORM_ENTRY(idx, node) \
  { .behavior_dev = DT_LABEL(DT_INST_PHANDLE_BY_IDX(node, bindings, idx)), \
    .param1 = COND_CODE_0(DT_INST_PHA_HAS_CELL_AT_IDX(node, bindings, idx, param1), (0), (DT_INST_PHA_BY_IDX(node, bindings, idx, param1))), \
    .param2 = COND_CODE_0(DT_INST_PHA_HAS_CELL_AT_IDX(node, bindings, idx, param2), (0), (DT_INST_PHA_BY_IDX(node, bindings, idx, param2))), \
  },

#define KP_INST(n) \
  static k_timeout_t behavior_tap_hold_config_##n##_gettime() { return K_MSEC(DT_INST_PROP(n, hold_ms)); } \
  static struct behavior_tap_hold_behaviors behavior_tap_hold_behaviors_##n = { \
    .tap = _TRANSFORM_ENTRY(0, n) \
    .hold = _TRANSFORM_ENTRY(1, n) \
  }; \
  ZMK_LISTENER(behavior_tap_hold_##n, behavior_tap_hold_listener); \
  ZMK_SUBSCRIPTION(behavior_mod_tap_##n, position_state_changed); \
  static struct behavior_tap_hold_config behavior_tap_hold_config_##n = { \
    .behaviors = &behavior_tap_hold_behaviors_##n, \
    .hold_ms = &behavior_tap_hold_config_##n##_gettime, \
  }; \
  static struct behavior_tap_hold_data behavior_tap_hold_data_##n { \
    .shared_data = behavior_tap_hold_shared_data_instance, \
  }; \
  DEVICE_AND_API_INIT(behavior_tap_hold_##n, DT_INST_LABEL(n), behavior_tap_hold_init, \
                      &behavior_tap_hold_data_##n, \
                      &behavior_tap_hold_config_##n, \
                      APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
                      &behavior_tap_hold_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)