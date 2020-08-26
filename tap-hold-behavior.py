from dataclasses import dataclass
from pprint import pp
from typing import Optional, Type

CAPTURED = 1

event_queue = []
mod_data = {}
mods = "ctrl shift alt gui".split()

def show():
    pp(mod_data)
    pp(event_queue)

@dataclass
class ModTapEvent():
    state: bool #true for down, false for up
    tap: str
    mod: str
    #maybe sync the events & data not by (mod,tap) but by keypos instead.

    def __str__(self):
        data = mod_data[self.tap, self.mod]
        key = data.behavior.get_key(data)
        if self.state:
            return f"D{key}"
        else:
            return f"U{key}"

@dataclass
class ModTapData():
    tap: str
    mod: str
    behavior: Type["Behavior"]
    is_decided: bool = False
    is_tap: Optional[bool] = None #true for tap, False for mod, None if undecided

    def __str__(self):
        return self

class Behavior:
    @staticmethod
    def get_key(data):
        assert data.is_decided
        if data.is_tap:
            return data.tap
        return data.mod

    @staticmethod
    def mt_keydown(data):
        pass

    @staticmethod
    def mt_keyup(data):
        pass

    @staticmethod
    def timer_passed(data):
        pass

    @staticmethod
    def other_keydown(data):
        pass

    @staticmethod
    def other_keyup(data):
        pass

class BalancedBehavior(Behavior):
    @staticmethod
    def mt_keyup(data):
        if not data.is_decided:
            data.is_decided = True
            data.is_tap = True

    @staticmethod
    def timer_passed(data):
        if not data.is_decided:
            data.is_decided = True
            data.is_tap = False

    @staticmethod
    def other_keyup(data):
        if not data.is_decided:
            data.is_decided = True
            data.is_tap = False


class TapPreferredBehavior(Behavior):
    @staticmethod
    def mt_keyup(data):
        if not data.is_decided:
            data.is_decided = True
            data.is_tap = True

    @staticmethod
    def timer_passed(data):
        if not data.is_decided:
            data.is_decided = True
            data.is_tap = False


class ModPreferredBehavior(Behavior):
    @staticmethod
    def mt_keyup(data):
        if not data.is_decided:
            data.is_decided = True
            data.is_tap = True

    @staticmethod
    def timer_passed(data):
        if not data.is_decided:
            data.is_decided = True
            data.is_tap = False

    @staticmethod
    def other_keydown(data):
        if not data.is_decided:
            data.is_decided = True
            data.is_tap = False

@dataclass
class KeyEvent():
    state: bool
    tap: str

    def __str__(self):
        if self.state:
            return f"D{self.tap}"
        else:
            return f"U{self.tap}"

def find_modtap_in_queue(tap, mod):
    for item in event_queue:
        if isinstance(item, ModTapEvent) and item.tap == tap and item.mod == mod:
            return item
    return None

def find_key_in_queue(tap):
    for item in event_queue:
        if isinstance(item, KeyEvent) and item.tap == tap:
            return item
    return None

def process_queue():
    result = ""
    while(event_queue):
        event = event_queue[0]
        if isinstance(event, ModTapEvent):
            #don't process the queue further if the next key to be sent is undecided.
            mt_data = mod_data[event.tap, event.mod]
            if not mt_data.is_decided:
                break

        event_queue.pop(0)
        # RELEASE EVENTS
        result += str(event)

        # clean up mod_data entry on mt keyup
        if isinstance(event, ModTapEvent) and not event.state:
            del mod_data[event.tap, event.mod]

    return result

def inform_active_modtaps_of_keydown():
    # we can safely send this to all modtaps, as the event will be ignored if the
    # behavior has already decided.
    for data in mod_data.values():
        data.behavior.other_keydown(data)

def inform_active_modtaps_of_keyup(keydown_event):
    # pass keyup event to all mods active since this key was pressed
    for q_event in event_queue:
        if q_event == keydown_event:
            break
        if not isinstance(q_event, ModTapEvent):
            continue
        data = mod_data[q_event.tap, q_event.mod]
        data.behavior.other_keyup(data)

def on_keymap_binding_pressed(tap, mod, behavior:Type[Behavior]=BalancedBehavior):
    assert process_queue() == ""
    event = ModTapEvent(True, tap, mod)
    # start timer
    inform_active_modtaps_of_keydown()

    mod_data[tap, mod] = data = ModTapData(tap, mod, behavior)
    data.behavior.mt_keydown(data)
    # return CAPTURED
    event_queue.append(event)
    return process_queue()

def on_keymap_binding_released(tap, mod):
    assert process_queue() == ""
    data = mod_data[tap, mod]
    data.behavior.mt_keyup(data)

    event = ModTapEvent(False, tap, mod)
    inform_active_modtaps_of_keyup(event)
    assert data.is_decided
    if data.is_tap:
        # release taps 'early' so we don't get unwanted key repeats
        result = process_queue() + str(event)
        del mod_data[tap, mod]
        return result
    else:
        event_queue.append(event)
        # return CAPTURED
        return process_queue()

def timer_passed(tap, mod):
    assert process_queue() == ""
    down_event = mod_data[tap, mod]
    down_event.is_tap = False
    down_event.is_decided = True
    return process_queue()

def other_keydown(key):
    assert process_queue() == ""
    event = KeyEvent(True, key)
    if not event_queue:
        return str(event)

    # nested keyup, alert behaviors
    for _, data in mod_data.items():
        data.behavior.other_keydown(data)

    # return CAPTURED

    event_queue.append(event)
    return process_queue()

def other_keyup(key):
    assert process_queue() == ""
    event = KeyEvent(False, key)

    if not event_queue:
        return str(event)

    keydown_event = find_key_in_queue(key)
    if keydown_event:
        inform_active_modtaps_of_keyup(keydown_event)
    else:
        if key not in mods:
            return str(event)

    event_queue.append(event)
    return process_queue()



def clear():
    assert event_queue == []
    assert mod_data == {}
    
def test_0():
    assert other_keydown("j") == "Dj"
    assert other_keyup("j") == "Uj"
    clear()


def test_1():
    assert on_keymap_binding_pressed("f", "shift") == ""
    assert on_keymap_binding_released("f", "shift") == "DfUf"
    clear()

def test_2():
    assert on_keymap_binding_pressed("f", "shift") == ""
    assert timer_passed("f", "shift") == "Dshift"
    assert on_keymap_binding_released("f", "shift") == "Ushift"
    clear()

def test_3a():
    assert other_keydown("ctrl") == "Dctrl"
    assert on_keymap_binding_pressed("f", "shift") == ""
    assert other_keyup("ctrl") == ""
    assert on_keymap_binding_released("f", "shift") == "DfUctrlUf"
    clear()

def test_3b():
    assert other_keydown("ctrl") == "Dctrl"
    assert on_keymap_binding_pressed("f", "shift") == ""
    assert other_keyup("ctrl") == ""
    assert timer_passed("f", "shift") == "DshiftUctrl"
    assert on_keymap_binding_released("f", "shift") == "Ushift"
    clear()

def test_3c():
    assert other_keydown("j") == "Dj"
    assert on_keymap_binding_pressed("f", "shift") == ""
    assert other_keyup("j") == "Uj"
    assert timer_passed("f", "shift") == "Dshift"
    assert on_keymap_binding_released("f", "shift") == "Ushift"
    clear()

def test_3d():
    assert other_keydown("j") == "Dj"
    assert on_keymap_binding_pressed("f", "shift") == ""
    assert other_keyup("j") == "Uj"
    assert timer_passed("f", "shift") == "Dshift"
    assert on_keymap_binding_released("f", "shift") == "Ushift"
    clear()


def test_balanced_4a():
    assert on_keymap_binding_pressed("f", "shift") == ""
    assert other_keydown("j") == ""
    assert timer_passed("f", "shift") == "DshiftDj"
    assert other_keyup("j") == "Uj"
    assert on_keymap_binding_released("f", "shift") == "Ushift"
    clear()

def test_balanced_4a1():
    assert on_keymap_binding_pressed("f", "shift") == ""
    assert other_keydown("j") == ""
    assert timer_passed("f", "shift") == "DshiftDj"
    assert on_keymap_binding_released("f", "shift") == "Ushift"
    assert other_keyup("j") == "Uj"
    clear()

def test_balanced_4b():
    assert on_keymap_binding_pressed("f", "shift") == ""
    assert other_keydown("j") == ""
    assert other_keyup("j") == "DshiftDjUj"
    assert timer_passed("f", "shift") == ""
    assert on_keymap_binding_released("f", "shift") == "Ushift"
    clear()

def test_balanced_4c():
    assert on_keymap_binding_pressed("f", "shift") == ""
    assert other_keydown("j") == ""
    assert other_keyup("j") == "DshiftDjUj"
    assert on_keymap_binding_released("f", "shift") == "Ushift"
    clear()

def test_balanced_4d():
    assert on_keymap_binding_pressed("f", "shift") == ""
    assert other_keydown("j") == ""
    assert on_keymap_binding_released("f", "shift") == "DfDjUf"
    assert other_keyup("j") == "Uj"
    clear()


def test_tap_preferred_4a():
    assert on_keymap_binding_pressed("f", "shift", TapPreferredBehavior) == ""
    assert other_keydown("j") == ""
    assert timer_passed("f", "shift") == "DshiftDj"
    assert other_keyup("j") == "Uj"
    assert on_keymap_binding_released("f", "shift") == "Ushift"
    clear()

def test_tap_preferred_4a1():
    assert on_keymap_binding_pressed("f", "shift", TapPreferredBehavior) == ""
    assert other_keydown("j") == ""
    assert timer_passed("f", "shift") == "DshiftDj"
    assert on_keymap_binding_released("f", "shift") == "Ushift"
    assert other_keyup("j") == "Uj"
    clear()

def test_tap_preferred_4b():
    assert on_keymap_binding_pressed("f", "shift", TapPreferredBehavior) == ""
    assert other_keydown("j") == ""
    assert other_keyup("j") == ""
    assert timer_passed("f", "shift") == "DshiftDjUj"
    assert on_keymap_binding_released("f", "shift") == "Ushift"
    clear()

def test_tap_preferred_4c():
    assert on_keymap_binding_pressed("f", "shift", TapPreferredBehavior) == ""
    assert other_keydown("j") == ""
    assert other_keyup("j") == ""
    assert on_keymap_binding_released("f", "shift") == "DfDjUjUf"
    clear()

def test_tap_preferred_4d():
    assert on_keymap_binding_pressed("f", "shift", TapPreferredBehavior) == ""
    assert other_keydown("j") == ""
    assert on_keymap_binding_released("f", "shift") == "DfDjUf"
    assert other_keyup("j") == "Uj"
    clear()


def test_mod_preferred_4a():
    assert on_keymap_binding_pressed("f", "shift", ModPreferredBehavior) == ""
    assert other_keydown("j") == "DshiftDj"
    assert timer_passed("f", "shift") == ""
    assert other_keyup("j") == "Uj"
    assert on_keymap_binding_released("f", "shift") == "Ushift"
    clear()

def test_mod_preferred_4a1():
    assert on_keymap_binding_pressed("f", "shift", ModPreferredBehavior) == ""
    assert other_keydown("j") == "DshiftDj"
    assert timer_passed("f", "shift") == ""
    assert on_keymap_binding_released("f", "shift") == "Ushift"
    assert other_keyup("j") == "Uj"
    clear()

def test_mod_preferred_4b():
    assert on_keymap_binding_pressed("f", "shift", ModPreferredBehavior) == ""
    assert other_keydown("j") == "DshiftDj"
    assert other_keyup("j") == "Uj"
    assert timer_passed("f", "shift") == ""
    assert on_keymap_binding_released("f", "shift") == "Ushift"
    clear()

def test_mod_preferred_4c():
    assert on_keymap_binding_pressed("f", "shift", ModPreferredBehavior) == ""
    assert other_keydown("j") == "DshiftDj"
    assert other_keyup("j") == "Uj"
    assert on_keymap_binding_released("f", "shift") == "Ushift"
    clear()

def test_mod_preferred_4d():
    assert on_keymap_binding_pressed("f", "shift", ModPreferredBehavior) == ""
    assert other_keydown("j") == "DshiftDj"
    assert on_keymap_binding_released("f", "shift") == "Ushift"
    assert other_keyup("j") == "Uj"
    clear()


def test_combo_5a():
    assert on_keymap_binding_pressed("f", "shift", BalancedBehavior) == ""
    assert on_keymap_binding_pressed("d", "ctrl", TapPreferredBehavior) == ""
    assert other_keydown("j") == ""
    assert other_keyup("j") == "Dshift"
    assert on_keymap_binding_released("f", "shift") == ""
    assert on_keymap_binding_released("d", "ctrl") == "DdDjUjUshiftUd"
    clear()


def test_combo_5b():
    assert on_keymap_binding_pressed("f", "shift", BalancedBehavior) == ""
    assert on_keymap_binding_pressed("d", "ctrl", TapPreferredBehavior) == ""
    assert on_keymap_binding_released("f", "shift") == "DfUf"
    assert other_keydown("j") == ""
    assert other_keyup("j") == ""
    assert on_keymap_binding_released("d", "ctrl") == "DdDjUjUd"
    clear()


def test_combo_5c():
    assert on_keymap_binding_pressed("f", "shift", BalancedBehavior) == ""
    assert on_keymap_binding_pressed("d", "ctrl", TapPreferredBehavior) == ""
    assert on_keymap_binding_released("d", "ctrl") == "DshiftDdUd"
    assert on_keymap_binding_released("f", "shift") == "Ushift"
    clear()

def test_combo_5d():
    assert on_keymap_binding_pressed("f", "shift", BalancedBehavior) == ""
    assert on_keymap_binding_pressed("d", "ctrl", TapPreferredBehavior) == ""
    assert on_keymap_binding_released("d", "ctrl") == "DshiftDdUd"
    assert on_keymap_binding_released("f", "shift") == "Ushift"
    clear()