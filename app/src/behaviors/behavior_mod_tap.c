/*
 * Copyright (c) 2020 Peter Johanson <peter@peterjohanson.com>
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_mod_tap

#include <device.h>
#include <drivers/behavior.h>
#include <logging/log.h>

#include <zmk/matrix.h>
#include <zmk/endpoints.h>
#include <zmk/event-manager.h>
#include <zmk/events/keycode-state-changed.h>
#include <zmk/events/modifiers-state-changed.h>
#include <zmk/hid.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

typedef k_timeout_t (*timer_func)();

#define ZMK_BHV_MOD_TAP_MAX_HELD 4
#define ZMK_BHV_MOD_TAP_MAX_PENDING_KC 4
// todo: make tapping_term configurable per-key
#define ZMK_BHV_MOD_TAP_TAPPING_TERM 200

struct active_mod_tap_item {
  u32_t keycode;
  u8_t mods;
  bool pending; // true if the mod/tap decision has been made
  zmk_mod_flags active_mods;
};

struct captured_keycode_state_change_item {
  struct keycode_state_changed* event;
  zmk_mod_flags active_mods;
};

struct behavior_mod_tap_data {
  struct k_timer timer;
  struct active_mod_tap_item active_mod_taps[ZMK_BHV_MOD_TAP_MAX_HELD];
  struct captured_keycode_state_change_item captured_keycode_events[ZMK_BHV_MOD_TAP_MAX_PENDING_KC];
};

bool have_pending_mods(char *label) {
  struct device *dev = device_get_binding(label);
  struct behavior_mod_tap_data *data = dev->driver_data;

  for (int i = 0; i < ZMK_BHV_MOD_TAP_MAX_HELD; i++) {
    if (data->active_mod_taps[i].mods) {
      LOG_DBG("Found pending mods for %d and keycode 0x%02X", data->active_mod_taps[i].mods, data->active_mod_taps[i].keycode);
      return true;
    }
  }

  return false;
}

struct captured_keycode_state_change_item* find_captured_keycode_event(struct behavior_mod_tap_data *data, u32_t keycode)
{
  for (int i = 0; i < ZMK_BHV_MOD_TAP_MAX_PENDING_KC; i++) {
    if (data->captured_keycode_events[i].event == NULL) {
      //skip mod-taps in list that have been released
      continue;
    }

    if (data->captured_keycode_events[i].event->keycode == keycode) {
      return &data->captured_keycode_events[i];
    }
  }

  return NULL;
}


void send_captured_keycode_events(struct behavior_mod_tap_data *data) {
  //send all captured key presses
  for (int j = 0; j < ZMK_BHV_MOD_TAP_MAX_PENDING_KC; j++) {
    if (data->captured_keycode_events[j].event == NULL) {
      continue;
    }

    struct keycode_state_changed *ev = data->captured_keycode_events[j].event;
    data->captured_keycode_events[j].event = NULL;
    data->captured_keycode_events[j].active_mods = 0;
    LOG_DBG("Re-sending latched key press for usage page 0x%02X keycode 0x%02X state %s", ev->usage_page, ev->keycode, (ev->state ? "pressed" : "released"));
    ZMK_EVENT_RELEASE(ev);
    k_msleep(10);
  }
}

zmk_mod_flags behavior_mod_tap_active_mods(struct behavior_mod_tap_data *data)
{
  zmk_mod_flags mods = 0;
  for (int i = 0; i < ZMK_BHV_MOD_TAP_MAX_HELD; i++) {
    mods |= data->active_mod_taps[i].mods;
  }
  return mods;
}

int behavior_mod_tap_capture_keycode_event(struct behavior_mod_tap_data *data, struct keycode_state_changed *ev)
{
  /* store a keycode event for later use */
  for (int i = 0; i < ZMK_BHV_MOD_TAP_MAX_PENDING_KC; i++) {
    // find the next unused keycode event struct
    if (data->captured_keycode_events[i].event != NULL) {
      continue;
    }

    data->captured_keycode_events[i].event = ev;
    data->captured_keycode_events[i].active_mods = behavior_mod_tap_active_mods(data);
    return 0;
  }

  return -ENOMEM;
}

void behavior_mod_tap_update_active_mods_state(struct behavior_mod_tap_data *data, zmk_mod_flags used_flags)
{
  for (int i = 0; i < ZMK_BHV_MOD_TAP_MAX_HELD; i++) {
    if ((data->active_mod_taps[i].mods & used_flags) == data->active_mod_taps[i].mods) {
      data->active_mod_taps[i].pending = false;
    }
  }
}

struct active_mod_tap_item* find_active_mod_tap_item(struct behavior_mod_tap_data *data, u32_t mods, u32_t keycode) {
  for (int i = 0; i < ZMK_BHV_MOD_TAP_MAX_HELD; i++) {
    struct active_mod_tap_item* item = &data->active_mod_taps[i];
    if (item->mods == mods && item->keycode == keycode) {
      return item;
    }
  }
  return NULL;
}

static void timer_stop_handler(struct k_timer *timer) {}
static void timer_expire_handler(struct k_timer *timer)
{
  // does this work in a timer?
  struct device *dev = device_get_binding(DT_INST_LABEL(0));
  struct behavior_mod_tap_data *data = dev->driver_data;

  LOG_DBG("Timer up, going to activate pending mods then send pending key presses");

  zmk_mod_flags active_mods = behavior_mod_tap_active_mods(data);
  zmk_hid_register_mods(active_mods);
  behavior_mod_tap_update_active_mods_state(data, active_mods);
  send_captured_keycode_events(data);
}


// How to pass context to subscription?!
int mod_tap_intercept_keycodes(const struct zmk_event_header *eh)
{
  // listen to all keycode-events to decide mod-tap behavior
  if (!is_keycode_state_changed(eh) || !have_pending_mods(DT_INST_LABEL(0))) {
    return 0;
  }

  struct device *dev = device_get_binding(DT_INST_LABEL(0));
  struct keycode_state_changed *ev = cast_keycode_state_changed(eh);
  struct behavior_mod_tap_data *data = dev->driver_data;
  if (ev->state) { // keydown
    // this is the moment another key is pressed during a mod-tap event.
    LOG_DBG("Have pending mods, capturing keycode 0x%02X event to resend later", ev->keycode);
    behavior_mod_tap_capture_keycode_event(data, ev);
    return ZMK_EV_EVENT_CAPTURED;
  }

  // keyup
  struct captured_keycode_state_change_item* pending_keycode = find_captured_keycode_event(data, ev->keycode);
  if (pending_keycode == NULL) { 
    // the release of a key that was pressed before any mod-tap key

    // todo: intercept mods and keep them active until hold/tap decision is made.
    // on hold decision: process intercepted mod keyups, start hold behavior
    // on tap decision: do tap behavior, process intercepted mod keyups
  } else {
    // the release of a key that was pressed after the mod-tap key
    LOG_DBG("Key released, going to activate mods 0x%02X then send pending key press for keycode 0x%02X",
            pending_keycode->active_mods, pending_keycode->event->keycode);

    zmk_hid_register_mods(pending_keycode->active_mods);
    behavior_mod_tap_update_active_mods_state(data, pending_keycode->active_mods);

    ZMK_EVENT_RELEASE(pending_keycode->event);
    k_msleep(10);

    pending_keycode->event = NULL;
    pending_keycode->active_mods = 0;
  }
  return 0;
}

ZMK_LISTENER(behavior_mod_tap, mod_tap_intercept_keycodes);
ZMK_SUBSCRIPTION(behavior_mod_tap, keycode_state_changed);

static int behavior_mod_tap_init(struct device *dev)
{
  struct behavior_mod_tap_data *data = dev->driver_data;

  k_timer_init(&data->timer, timer_expire_handler, timer_stop_handler);
  //k_timer_user_data_set(&data->timer, (void*)dev->config_info);

	return 0;
};

static int on_modtap_key_pressed(struct device *dev, u32_t position, u32_t mods, u32_t keycode)
{
  // mod-tap key pressed
  struct behavior_mod_tap_data *data = dev->driver_data;
  LOG_DBG("mods: %d, keycode: 0x%02X", mods, keycode);
  for (int i = 0; i < ZMK_BHV_MOD_TAP_MAX_HELD; i++) {
    if (data->active_mod_taps[i].mods != 0) {
      continue;
    }

    zmk_mod_flags active_mods = behavior_mod_tap_active_mods(data);

    data->active_mod_taps[i].active_mods = active_mods;
    data->active_mod_taps[i].mods = mods;
    data->active_mod_taps[i].keycode = keycode;
    data->active_mod_taps[i].pending = true;

    return 0;
  }

  LOG_WRN("Failed to record mod-tap activation, at maximum concurrent mod-tap activations");

  return -ENOMEM;
}

static int on_modtap_key_released(struct device *dev, u32_t position, u32_t mods, u32_t keycode)
{
  // mod-tap key released
  struct behavior_mod_tap_data *data = dev->driver_data;
  LOG_DBG("mods: %d, keycode: %d", mods, keycode);

  struct active_mod_tap_item *item = find_active_mod_tap_item(data, mods, keycode);
  if(item == NULL) {
    return 0;
  }

  if (item->pending) { //trigger tap behavior
    LOG_DBG("Sending un-triggered mod-tap for keycode: 0x%02X", keycode);

    if (item->active_mods) {
      LOG_DBG("Registering recorded active mods captured when mod-tap initially activated: 0x%02X", item->active_mods);
      behavior_mod_tap_update_active_mods_state(data, item->active_mods);
      zmk_hid_register_mods(item->active_mods);
    }

    struct keycode_state_changed *key_press = create_keycode_state_changed(USAGE_KEYPAD, item->keycode, true);
    ZMK_EVENT_RAISE_AFTER(key_press, behavior_mod_tap);
    k_msleep(10);

    send_captured_keycode_events(data);

    struct keycode_state_changed *key_release = create_keycode_state_changed(USAGE_KEYPAD, keycode, false);
    LOG_DBG("Sending un-triggered mod-tap release for keycode: 0x%02X", keycode);
    ZMK_EVENT_RAISE_AFTER(key_release, behavior_mod_tap);
    k_msleep(10);

    if (item->active_mods) {
      LOG_DBG("Unregistering recorded active mods captured when mod-tap initially activated: 0x%02X", item->active_mods);
      zmk_hid_unregister_mods(item->active_mods);
      zmk_endpoints_send_report(USAGE_KEYPAD);
    } 
  } else { // release mod-behavior
    LOG_DBG("Releasing triggered mods: %d", mods);
    zmk_hid_unregister_mods(mods);
    zmk_endpoints_send_report(USAGE_KEYPAD);
  }

  item->mods = 0;
  item->keycode = 0;
  item->active_mods = 0;

  LOG_DBG("Removing mods %d from active_mods for other held mod-taps", mods);
  for (int j = 0; j < ZMK_BHV_MOD_TAP_MAX_HELD; j++) {
    if (data->active_mod_taps[j].active_mods & mods) {
      LOG_DBG("Removing 0x%02X from active mod tap mods 0x%02X keycode 0x%02X", mods, data->active_mod_taps[j].mods, data->active_mod_taps[j].keycode);
      data->active_mod_taps[j].active_mods &= ~mods;
    }
  }

  return 0;
}

static const struct behavior_driver_api behavior_mod_tap_driver_api = {
  .binding_pressed = on_modtap_key_pressed,
  .binding_released = on_modtap_key_released,
};

static const struct behavior_mod_tap_config behavior_mod_tap_config = {};

static struct behavior_mod_tap_data behavior_mod_tap_data;

DEVICE_AND_API_INIT(behavior_mod_tap, DT_INST_LABEL(0), behavior_mod_tap_init,
                    &behavior_mod_tap_data,
                    &behavior_mod_tap_config,
                    APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                    &behavior_mod_tap_driver_api);
