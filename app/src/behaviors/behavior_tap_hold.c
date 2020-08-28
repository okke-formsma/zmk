/*
 * Copyright (c) 2020 Cody McGinnis, Okke Formsma
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

/************************************************************  DATA SETUP */
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

// this data is specific to a configured behavior
struct behavior_tap_hold_data {
  //todo: add timestamp when keydown was seen
  struct k_timer timer;
  struct k_work work;

  int first_active_tap_hold;
  struct active_tap_hold active_tap_holds[ZMK_BHV_TAP_HOLD_MAX_HELD];
  int first_captured_position_event;
  struct position_state_changed* captured_position_events[ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC];
};

struct behavior_tap_hold_config {
  timer_func tapping_term_ms;
  struct behavior_tap_hold_behaviors* behaviors;
};


/************************************************************  CAPTURED POSITION HELPER FUNCTIONS */
void init_captured_position_events(struct behavior_tap_hold_data *data) {
  data->first_captured_position_event = 0;
  for (int i = 0; i < ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC; i++) {
    data->captured_position_events[i] = NULL;
  }
}

int capture_position_event(struct behavior_tap_hold_data *data, struct position_state_changed* event) {
  for (int i = data->first_captured_position_event; i < ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC; i++) {
    if (data->captured_position_events[i] == NULL) {
      data->captured_position_events[i] = event;
      return 0;
    }
  }
  return -ENOMEM;
}

struct position_state_changed* find_captured_position_event(struct behavior_tap_hold_data *data, u32_t position)
{
  // loop in reverse order so the last keydown event is found if events
  // are queued for the same position.
  for (int i = ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC-1; i >= 0; i--) {
    struct position_state_changed* next_captured_event = data->captured_position_events[i];
    if (next_captured_event == NULL) {
      continue;
    }

    if (next_captured_event->position == position) {
      return next_captured_event;
    }
  }

  return NULL;
}

void release_captured_positions(struct behavior_tap_hold_data *data) {
  // copy the events queue on the stack, as the next active mod-tap reuses the same global array.
  // alternative is to keep an array on the heap for each behavior instance or a linked-list.
  struct position_state_changed* events_queue[ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC];
  memcpy(events_queue, data->captured_position_events, sizeof(events_queue));
  int next_event = data->first_captured_position_event;
  init_captured_position_events(data);
  
  for(; next_event < ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC; next_event++) {
    struct position_state_changed* captured_position = events_queue[next_event];
    if(captured_position == NULL) {
      return;
    }
    LOG_DBG("Releasing key press for position 0x%02X state %s", captured_position->position, (captured_position->state ? "pressed" : "released"));
    ZMK_EVENT_RAISE(captured_position);
    k_msleep(10);
  }
}


/************************************************************  ACTIVE TAP HOLD HELPER FUNCTIONS */
/* returns the current undecided tap_hold key or NULL if none are active. */
struct active_tap_hold* undecided_tap_hold(struct behavior_tap_hold_data *data) {
  for (int i = data->first_active_tap_hold; i < ZMK_BHV_TAP_HOLD_MAX_HELD; i++) {
    struct active_tap_hold* active_tap_hold = &data->active_tap_holds[i];
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
  data->first_active_tap_hold = 0;
  for (int i = 0; i < ZMK_BHV_TAP_HOLD_MAX_HELD; i++) {
    struct active_tap_hold* active_tap_hold = &data->active_tap_holds[i];
    active_tap_hold->position = -1;
    active_tap_hold->is_decided = 0;
  }
}

struct active_tap_hold* find_tap_hold(struct behavior_tap_hold_data *data, u32_t position) 
{
  for (int i = data->first_active_tap_hold; i < ZMK_BHV_TAP_HOLD_MAX_HELD; i++) {
    struct active_tap_hold* active_tap_hold = &data->active_tap_holds[i];
    if (active_tap_hold->position == position) {
      return active_tap_hold;
    }
  }
  return NULL;
}

int clear_tap_hold(struct behavior_tap_hold_data *data, u32_t position) 
{
  struct active_tap_hold* active_tap_hold = find_tap_hold(data, position);
  if (active_tap_hold == NULL) {
    return -ENOMEM;
  }
  active_tap_hold->position = -1;
  active_tap_hold->is_decided = 0;
  return 0;
}

/************************************************************  TIMER FUNCTIONS */
static void behavior_tap_hold_timer_work(struct k_work *item)
{
  //struct behavior_tap_hold_data *data = CONTAINER_OF(item, struct behavior_tap_hold_data, work);
  //todo:
  // make decision
  // bubble required key events
}

K_WORK_DEFINE(behavior_tap_hold_work, behavior_tap_hold_timer_work);

static void timer_handler(struct k_timer *timer)
{
  k_work_submit(&behavior_tap_hold_work);
}

static int behavior_tap_hold_init(struct device *dev)
{
  struct behavior_tap_hold_data *data = dev->driver_data;
  init_active_tap_holds(data);
  init_captured_position_events(data);
  k_timer_init(&data->timer, timer_handler, NULL);
  k_timer_user_data_set(&data->timer, (void*)dev->config_info);
  k_work_init(&data->work, behavior_tap_hold_timer_work);
  return 0;
}

/************************************************************  tap_hold_binding and key handlers */
static int on_tap_hold_binding_pressed(struct device *dev, u32_t position, u32_t _, u32_t __)
{
  struct behavior_tap_hold_data *data = dev->driver_data;
  const struct behavior_tap_hold_config *cfg = dev->config_info;

  LOG_DBG("key down: tap-hold on position: %d", position);
  LOG_DBG("timer %p started", &data->timer);
  k_timer_start(&data->timer, cfg->tapping_term_ms(), K_NO_WAIT);

  return 0;
}

static int on_tap_hold_binding_released(struct device *dev, u32_t position, u32_t _, u32_t __)
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
  struct position_state_changed* ev = cast_position_state_changed(eh);
  if(!ev->state && tap_hold->position == ev->position) {
    // don't capture this event so on_tap_hold_binding_released will be called
    LOG_DBG("Key up event for currently active tap-hold on %d", tap_hold->position);
    return 0;
  }
  
  if (ev->state) { // key down
    LOG_DBG("Pending tap-hold. Capturing position %d down event", ev->position);
    if(!tap_hold->is_decided) {
      //only for mod-preferred behavior
    }
  } else { // key up 
    if((find_captured_position_event(data, ev->position)) != NULL) {
      LOG_DBG("Pending tap-hold. Capturing position %d up event", ev->position);
      if(!tap_hold->is_decided) {
        tap_hold->is_decided = 1;
        tap_hold->is_hold = 1;
      }
    } else {
      // no key-down event seen while the current mod-tap is active.
      //todo: allow key-up events for non-mod keys pressed before the TH was pressed.
      // see scenario 3c/3d vs 3a/3b.
    }
  }

  capture_position_event(data, ev);
  return ZMK_EV_EVENT_CAPTURED;
}


static const struct behavior_driver_api behavior_tap_hold_driver_api = {
  .binding_pressed = on_tap_hold_binding_pressed,
  .binding_released = on_tap_hold_binding_released,
};

#endif

#define _TRANSFORM_ENTRY(idx, node) \
  { .behavior_dev = DT_LABEL(DT_INST_PHANDLE_BY_IDX(node, bindings, idx)), \
    .param1 = COND_CODE_0(DT_INST_PHA_HAS_CELL_AT_IDX(node, bindings, idx, param1), (0), (DT_INST_PHA_BY_IDX(node, bindings, idx, param1))), \
    .param2 = COND_CODE_0(DT_INST_PHA_HAS_CELL_AT_IDX(node, bindings, idx, param2), (0), (DT_INST_PHA_BY_IDX(node, bindings, idx, param2))), \
  },

#define KP_INST(n) \
  static k_timeout_t behavior_tap_hold_config_##n##_gettime() { return K_MSEC(DT_INST_PROP(n, tapping_term_ms)); } \
  static struct behavior_tap_hold_behaviors behavior_tap_hold_behaviors_##n = { \
    .tap = _TRANSFORM_ENTRY(0, n) \
    .hold = _TRANSFORM_ENTRY(1, n) \
  }; \
  ZMK_LISTENER(behavior_tap_hold_##n, behavior_tap_hold_listener); \
  ZMK_SUBSCRIPTION(behavior_tap_hold_##n, position_state_changed); \
  static struct behavior_tap_hold_config behavior_tap_hold_config_##n = { \
    .behaviors = &behavior_tap_hold_behaviors_##n, \
    .tapping_term_ms = &behavior_tap_hold_config_##n##_gettime, \
  }; \
  static struct behavior_tap_hold_data behavior_tap_hold_data_##n; \
  DEVICE_AND_API_INIT(behavior_tap_hold_##n, DT_INST_LABEL(n), behavior_tap_hold_init, \
                      &behavior_tap_hold_data_##n, \
                      &behavior_tap_hold_config_##n, \
                      APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
                      &behavior_tap_hold_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)