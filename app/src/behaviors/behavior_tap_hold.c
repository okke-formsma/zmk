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

#define TH_KEY_DOWN 0
#define TH_KEY_UP 1
#define TH_OTHER_KEY_DOWN 2
#define TH_OTHER_KEY_UP 3
#define TH_TIMER_EVENT 4

// increase if you have keyboard with more keys.
#define TH_POSITION_NOT_USED 9999 

// todo: this should be done in config
#define CURRENT_TYPE TH_TYPE_BALANCED

#if DT_NODE_EXISTS(DT_DRV_INST(0))
struct behavior_tap_hold_behaviors {
  struct zmk_behavior_binding tap;
  struct zmk_behavior_binding hold;
};

typedef k_timeout_t (*timer_func)();

// this data is specific to a configured behavior (which may be multiple tap-hold keys)
struct behavior_tap_hold_data {
  struct k_timer timer;
  struct k_work work;
};

struct behavior_tap_hold_config {
  timer_func tapping_term_ms;
  struct behavior_tap_hold_behaviors* behaviors;
};

// this data is specific for each tap-hold
struct active_tap_hold {
  s32_t position;
  bool is_decided; //todo remove
  bool is_hold;
  const struct behavior_tap_hold_config *config;
};

struct active_tap_hold* undecided_tap_hold = NULL;
struct active_tap_hold active_tap_holds[ZMK_BHV_TAP_HOLD_MAX_HELD] = {{
  .position = TH_POSITION_NOT_USED,
  .is_decided = false,
  .is_hold = false,
  .config = NULL,
}};
struct position_state_changed* captured_position_events[ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC] = {NULL};

/************************************************************  CAPTURED POSITION HELPER FUNCTIONS */
int capture_position_event(struct position_state_changed* event) {
  for (int i = 0; i < ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC; i++) {
    if (captured_position_events[i] == NULL) {
      captured_position_events[i] = event;
      return 0;
    }
  }
  return -ENOMEM;
}

struct position_state_changed* find_captured_position_event(u32_t position)
{
  struct position_state_changed *last_match = NULL;
  for (int i = 0; i < ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC; i++) {
    struct position_state_changed* event = captured_position_events[i];
    if (event == NULL) {
      return last_match;
    }

    if (event->position == position) {
      last_match = event;
    }
  }

  return last_match;
}

void release_captured_positions() {
  if (undecided_tap_hold != NULL) {
    return;
  }

  // We use a trick to prevent copying the captured_position_events array.
  //
  // Events for different mod-tap instances are separated by a NULL pointer.
  //
  // The first event popped will never be catched by the next active tap-hold
  // because to start capturing a mod-tap-key-down event must first completely
  // go through the events queue.
  // 
  // Example of this release process;
  // [mt2_down, k1_down, k1_up, mt2_up, null, ...]
  //  ^
  // mt2_down position event isn't captured because no tap-hold is active.
  // mt2_down behavior event is handled, now we have an undecided tap-hold
  // [null, k1_down, k1_up, mt2_up, null, ...]
  //        ^
  // k1_down  is captured by the mt2 mod-tap
  // !note that searches for find_captured_position_event by the mt2 behavior will stop at the first null encountered
  // [mt1_down, null, k1_up, mt2_up, null, ...]
  //                  ^
  // k1_up event is captured by the new tap-hold:
  // [k1_down, k1_up, null, mt2_up, null, ...]
  //                        ^
  // mt2_up event is not captured but causes release of mt2 behavior
  // [k1_down, k1_up, null, null, null, ...]
  // now mt2 will start releasing it's own captured positions.
  for(int i = 0; i < ZMK_BHV_TAP_HOLD_MAX_CAPTURED_KC; i++) {
    struct position_state_changed* captured_position = captured_position_events[i];
    if(captured_position == NULL) {
      return;
    }
    captured_position_events[i] = NULL;
    LOG_DBG("Releasing key position event for position %d %s", captured_position->position, (captured_position->state ? "pressed" : "released"));
    ZMK_EVENT_RAISE(captured_position);
    if(undecided_tap_hold != NULL) {
      // sleep if no tap hold is active so we're releasing events in the correct order.
      k_msleep(10);
    }
  }
}


/************************************************************  ACTIVE TAP HOLD HELPER FUNCTIONS */
struct active_tap_hold* find_tap_hold(u32_t position) 
{
  for (int i = 0; i < ZMK_BHV_TAP_HOLD_MAX_HELD; i++) {
    if (active_tap_holds[i].position == position) {
      return &active_tap_holds[i];
    }
  }
  return NULL;
}

int store_tap_hold(u32_t position, const struct behavior_tap_hold_config* config) 
{
  for (int i = 0; i < ZMK_BHV_TAP_HOLD_MAX_HELD; i++) {
    if (active_tap_holds[i].position == TH_POSITION_NOT_USED) {
      active_tap_holds[i].position = position;
      active_tap_holds[i].is_decided = 0;
      active_tap_holds[i].config = config;
      return 0;
    }
  }
  return -ENOMEM;
}

void clear_tap_hold(u32_t position) 
{
  struct active_tap_hold* active_tap_hold = find_tap_hold(position);
  if (active_tap_hold == NULL) {
    LOG_DBG("ERROR clearing tap hold on position %d that was already cleared", position);
    return;
  }
  active_tap_hold->position = TH_POSITION_NOT_USED;
  return;
}

void decide_tap_hold(struct active_tap_hold * tap_hold, u32_t event)
{
  if (tap_hold->is_decided) {
    return;
  }

  if(tap_hold != undecided_tap_hold) {
    LOG_DBG("ERROR found undecided tap hold that is not the active tap hold");
  }

  //todo: add other behaviors 
  bool decided = true;
  switch(event) {
    case TH_KEY_UP: 
      tap_hold->is_hold = 1;
      break;
    case TH_OTHER_KEY_UP:
      tap_hold->is_hold = 0;
      break;
    case TH_TIMER_EVENT:
      tap_hold->is_hold = 1;
      break;
    default:
      decided = false;
  }

  if(!decided) {
    return;
  }

  LOG_DBG("decided tap-hold for position: %d %s", tap_hold->position, tap_hold->is_hold?"hold":"tap");
  struct active_tap_hold * decided_tap_hold = undecided_tap_hold;
  undecided_tap_hold = NULL;

  struct zmk_behavior_binding *behavior;
  if (decided_tap_hold->is_hold) {
    behavior = &decided_tap_hold->config->behaviors->hold;
  } else {
    behavior = &decided_tap_hold->config->behaviors->tap;
  }
  struct device *behavior_device = device_get_binding(behavior->behavior_dev);
  behavior_keymap_binding_pressed(behavior_device,  tap_hold->position, behavior->param1, behavior->param2);
  release_captured_positions();
}

/************************************************************  tap_hold_binding and key handlers */
static int behavior_tap_hold_init(struct device *dev)
{
  return 0;
}

static int on_tap_hold_binding_pressed(struct device *dev, u32_t position, u32_t _, u32_t __)
{
  struct behavior_tap_hold_data *data = dev->driver_data;
  const struct behavior_tap_hold_config *cfg = dev->config_info;

  store_tap_hold(position, cfg);

  //todo: once we get timing info for keypresses, start the timer relative to the original keypress
  //todo: maybe init timer and work here to be able to refer to the correct mod-tap?

  LOG_DBG("key down: tap-hold on position: %d", position);
  LOG_DBG("timer %p started", &data->timer);
  k_timer_start(&data->timer, cfg->tapping_term_ms(), K_NO_WAIT);
  return 0;
}

static int on_tap_hold_binding_released(struct device *dev, u32_t position, u32_t _, u32_t __)
{
  struct behavior_tap_hold_data *data = dev->driver_data;
  //const struct behavior_tap_hold_config *cfg = dev->config_info;
  k_timer_stop(&data->timer);
  decide_tap_hold(find_tap_hold(position), TH_KEY_UP);
  return 0;
}

static const struct behavior_driver_api behavior_tap_hold_driver_api = {
  .binding_pressed = on_tap_hold_binding_pressed,
  .binding_released = on_tap_hold_binding_released,
};

// How to pass context to subscription?!
// this forces us to use global variables to keep track of the current tap-hold...
int behavior_tap_hold_listener(const struct zmk_event_header *eh)
{
  //struct behavior_tap_hold_data *data = CONTAINER_OF(zmk_event_header, struct behavior_tap_hold_data, listener);
  //struct device *dev = device_get_binding(DT_INST_LABEL(0));
  //struct behavior_tap_hold_data *data = dev->driver_data;
  if (!is_position_state_changed(eh) || undecided_tap_hold == NULL) {
    return 0;
  }
  struct position_state_changed* ev = cast_position_state_changed(eh);
  if(!ev->state && undecided_tap_hold->position == ev->position) {
    // don't capture this event so on_tap_hold_binding_released will be called
    LOG_DBG("Key up event for currently active tap-hold on %d", undecided_tap_hold->position);
    return 0;
  }


  if (ev->state) { 
    // key down
    LOG_DBG("Pending tap-hold %d. Capturing position %d down event", undecided_tap_hold->position, ev->position);
    capture_position_event(ev);
    decide_tap_hold(undecided_tap_hold, TH_OTHER_KEY_DOWN);
    return ZMK_EV_EVENT_CAPTURED;
  } 
  
  // key up
  struct position_state_changed* captured_key_down = find_captured_position_event(ev->position); 
  if(captured_key_down != NULL) {
    LOG_DBG("Pending tap-hold %d. Capturing position %d up event", undecided_tap_hold->position, ev->position);
    capture_position_event(ev);
    decide_tap_hold(undecided_tap_hold, TH_OTHER_KEY_UP);
    return ZMK_EV_EVENT_CAPTURED;
  } 

  LOG_DBG("Pending tap-hold %d. Not capturing position %d up event because this tap-hold did not observe the down event.", undecided_tap_hold->position, ev->position);
  // no key-down event seen while the current mod-tap is active.
  // todo: allow key-up events for non-mod keys pressed before the TH was pressed.
  // see scenario 3c/3d vs 3a/3b.
  return 0;
}


ZMK_LISTENER(behavior_tap_hold, behavior_tap_hold_listener);
ZMK_SUBSCRIPTION(behavior_tap_hold, position_state_changed);

/************************************************************  TIMER FUNCTIONS */
static struct behavior_tap_hold_data behavior_tap_hold_data;

static void behavior_tap_hold_timer_work_handler(struct k_work *item)
{
  //todo: what happens if the timer runs out just as the key-up was processed?
  //struct behavior_tap_hold_data *data = CONTAINER_OF(item, struct behavior_tap_hold_data, work);
  decide_tap_hold(undecided_tap_hold, TH_TIMER_EVENT);
}

K_WORK_DEFINE(behavior_tap_hold_timer_work, behavior_tap_hold_timer_work_handler);

static void behavior_tap_hold_timer_expiry_handler(struct k_timer *timer)
{
  k_work_submit(&behavior_tap_hold_timer_work);
}

K_TIMER_DEFINE(behavior_tap_hold_timer, behavior_tap_hold_timer_expiry_handler, NULL);

#endif



/************************************************************ NODE CONFIG */
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
  static struct behavior_tap_hold_config behavior_tap_hold_config_##n = { \
    .behaviors = &behavior_tap_hold_behaviors_##n, \
    .tapping_term_ms = &behavior_tap_hold_config_##n##_gettime, \
  }; \
  DEVICE_AND_API_INIT(behavior_tap_hold_##n, DT_INST_LABEL(n), behavior_tap_hold_init, \
                      &behavior_tap_hold_data, \
                      &behavior_tap_hold_config_##n, \
                      APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, \
                      &behavior_tap_hold_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)