from dataclasses import dataclass
from pprint import pp
from typing import Optional, Type

event_queue = []
mod_data = {}
mods = "ctrl shift alt gui".split()

def clear():
    mod_data.clear()
    event_queue.clear()

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
    return False

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

        # "raise event"
        result += str(event)

        # clean up mod_data entry on mt keyup
        if isinstance(event, ModTapEvent) and not event.state:
            del mod_data[event.tap, event.mod]

    return result

def mt_keydown(tap, mod, behavior=BalancedBehavior):
    event = ModTapEvent(True, tap, mod)
    event_queue.append(event)
    mod_data[tap, mod] = data = ModTapData(tap, mod, behavior)
    data.behavior.mt_keydown(data)
    #start timer
    return process_queue()

def mt_keyup(tap, mod):
    data = mod_data[tap, mod]
    data.behavior.mt_keyup(data)

    event = ModTapEvent(False, tap, mod)
    event_queue.append(event)

    return process_queue()

def timer_passed(tap, mod):
    down_event = mod_data[tap, mod]
    down_event.is_tap = False
    down_event.is_decided = True
    return process_queue()

def other_keydown(key):
    event = KeyEvent(True, key)
    if not event_queue:
        return str(event)
    event_queue.append(event)
    return ""

def other_keyup(key):
    event = KeyEvent(False, key)

    if not event_queue:
        return str(event)

    keydown_event = find_key_in_queue(key)
    if keydown_event:
        # todo: only do this for mods active before the keydown event.
        # nested keyup, alert behaviors
        for _, data in mod_data.items():
            data.behavior.other_keyup(data)
    else:
        # the key was pressed before the mod-tap
        if not key in mods:
            # process key-up events for non-mod keys immediately
            return str(event)

    event_queue.append(event)
    return process_queue()



def test_1():
    clear()
    assert mt_keydown("f", "shift") == ""
    assert mt_keyup("f", "shift") == "DfUf"

def test_2():
    clear()
    assert mt_keydown("f", "shift") == ""
    assert timer_passed("f", "shift") == "Dshift"
    assert mt_keyup("f", "shift") == "Ushift"

def test_3a():
    clear()
    assert other_keydown("ctrl") == "Dctrl"
    assert mt_keydown("f", "shift") == ""
    assert other_keyup("ctrl") == ""
    assert mt_keyup("f", "shift") == "DfUctrlUf"

def test_3b():
    clear()
    assert other_keydown("ctrl") == "Dctrl"
    assert mt_keydown("f", "shift") == ""
    assert other_keyup("ctrl") == ""
    assert timer_passed("f", "shift") == "DshiftUctrl"
    assert mt_keyup("f", "shift") == "Ushift"

def test_3c():
    clear()
    assert other_keydown("j") == "Dj"
    assert mt_keydown("f", "shift") == ""
    assert other_keyup("j") == "Uj"
    assert timer_passed("f", "shift") == "Dshift"
    assert mt_keyup("f", "shift") == "Ushift"

def test_balanced_4a():
    clear()
    assert mt_keydown("f", "shift") == ""
    assert other_keydown("j") == ""
    assert timer_passed("f", "shift") == "DshiftDj"
    assert other_keyup("j") == "Uj"
    assert mt_keyup("f", "shift") == "Ushift"

def test_balanced_4a1():
    clear()
    assert mt_keydown("f", "shift") == ""
    assert other_keydown("j") == ""
    assert timer_passed("f", "shift") == "DshiftDj"
    assert mt_keyup("f", "shift") == "Ushift"
    assert other_keyup("j") == "Uj"

def test_balanced_4b():
    clear()
    assert mt_keydown("f", "shift") == ""
    assert other_keydown("j") == ""
    assert other_keyup("j") == "DshiftDjUj"
    assert timer_passed("f", "shift") == ""
    assert mt_keyup("f", "shift") == "Ushift"

def test_balanced_4c():
    clear()
    assert mt_keydown("f", "shift") == ""
    assert other_keydown("j") == ""
    assert other_keyup("j") == "DshiftDjUj"
    assert mt_keyup("f", "shift") == "Ushift"

def test_balanced_4d():
    clear()
    assert mt_keydown("f", "shift") == ""
    assert other_keydown("j") == ""
    assert mt_keyup("f", "shift") == "DfDjUf"
    assert other_keyup("j") == "Uj"
