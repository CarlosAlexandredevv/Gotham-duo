#include "input_trigger.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "CHECK failed (%s:%d): %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                        \
        }                                                                        \
    } while (false)

namespace {

using gotham::ControllerButtons;
using gotham::InputConfig;
using gotham::InputTrigger;
using gotham::TriggerAction;

void TestF9EdgeToggle() {
    InputTrigger trigger;
    std::array<ControllerButtons, 4> slots{};

    // Rising edge in Solo -> ToggleToDuo; holding F9 does not retrigger.
    CHECK(trigger.Update(false, false, slots, 0) == TriggerAction::None);
    CHECK(trigger.Update(false, true, slots, 10) == TriggerAction::ToggleToDuo);
    CHECK(trigger.Update(true, true, slots, 20) == TriggerAction::None);

    // Release F9, then rising edge in Duo -> ToggleToSolo.
    CHECK(trigger.Update(true, false, slots, 30) == TriggerAction::None);
    CHECK(trigger.Update(true, true, slots, 40) == TriggerAction::ToggleToSolo);
    CHECK(trigger.Update(false, true, slots, 50) == TriggerAction::None);

    // F9 toggle syncs prev_start_down_ so an already-held Start on slot 1 does not
    // immediately trigger ToggleToDuo on the next tick.
    slots[1] = {true, true, false};
    CHECK(trigger.Update(true, false, slots, 60) == TriggerAction::None);
    CHECK(trigger.Update(true, true, slots, 70) == TriggerAction::ToggleToSolo);
    CHECK(trigger.Update(false, false, slots, 80) == TriggerAction::None);
}

void TestController2StartEntersDuo() {
    InputTrigger trigger;
    std::array<ControllerButtons, 4> slots{};

    // Slot 0 (P1) Start must be ignored when p2_controller_index is -1.
    slots[0] = {true, true, false};
    CHECK(trigger.Update(false, false, slots, 100) == TriggerAction::None);

    // Slot 1 (Controller 2) Start rising edge enters Duo.
    slots[1] = {true, true, false};
    CHECK(trigger.Update(false, false, slots, 110) == TriggerAction::ToggleToDuo);

    // Holding Start in Duo does nothing.
    CHECK(trigger.Update(true, false, slots, 120) == TriggerAction::None);

    // Disconnecting slot 1 clears state; reconnecting with Start held triggers on rising edge.
    slots[1] = {false, false, false};
    CHECK(trigger.Update(false, false, slots, 130) == TriggerAction::None);
    slots[1] = {true, true, false};
    CHECK(trigger.Update(false, false, slots, 140) == TriggerAction::ToggleToDuo);
}

void TestHoldBackStartExitsDuoAfter800Ms() {
    InputTrigger trigger;
    std::array<ControllerButtons, 4> slots{};
    slots[1] = {true, false, false};

    // Holding only Back or only Start does not exit Duo.
    slots[1].back = true;
    CHECK(trigger.Update(true, false, slots, 1000) == TriggerAction::None);
    CHECK(trigger.Update(true, false, slots, 2000) == TriggerAction::None);

    // Holding both Back + Start starts the 800ms timer.
    slots[1].start = true;
    CHECK(trigger.Update(true, false, slots, 2100) == TriggerAction::None);
    CHECK(trigger.Update(true, false, slots, 2899) == TriggerAction::None);
    CHECK(trigger.Update(true, false, slots, 2900) == TriggerAction::ToggleToSolo);

    // In Solo now: still holding Start/Back must not immediately re-enter Duo until both are released.
    CHECK(trigger.Update(false, false, slots, 2910) == TriggerAction::None);
    slots[1].back = false;
    CHECK(trigger.Update(false, false, slots, 2920) == TriggerAction::None);
    slots[1].start = false;
    CHECK(trigger.Update(false, false, slots, 2930) == TriggerAction::None);

    // After full release, pressing Start again enters Duo.
    slots[1].start = true;
    CHECK(trigger.Update(false, false, slots, 2940) == TriggerAction::ToggleToDuo);

    // Releasing combo early resets timer.
    slots[1] = {true, true, true};
    CHECK(trigger.Update(true, false, slots, 3000) == TriggerAction::None);
    slots[1].back = false;
    CHECK(trigger.Update(true, false, slots, 3500) == TriggerAction::None);
    slots[1].back = true;
    CHECK(trigger.Update(true, false, slots, 3600) == TriggerAction::None);
    CHECK(trigger.Update(true, false, slots, 4399) == TriggerAction::None);
    CHECK(trigger.Update(true, false, slots, 4400) == TriggerAction::ToggleToSolo);
}

void TestDisabledControllerAndExplicitSlot() {
    InputTrigger trigger;
    InputConfig cfg{};
    cfg.allow_controller = false;
    trigger.SetConfig(cfg);
    CHECK(!trigger.config().allow_controller);

    std::array<ControllerButtons, 4> slots{};
    slots[1] = {true, true, false};
    CHECK(trigger.Update(false, false, slots, 100) == TriggerAction::None);

    // F9 still works even when allow_controller is false.
    CHECK(trigger.Update(false, true, slots, 110) == TriggerAction::ToggleToDuo);
    CHECK(trigger.Update(true, false, slots, 120) == TriggerAction::None);

    // Explicit slot 0 enables Controller 1 and ignores Controller 2 (slot 1).
    cfg.allow_controller = true;
    cfg.p2_controller_index = 0;
    cfg.hold_back_start_ms = 500;
    trigger.SetConfig(cfg);

    slots = {};
    slots[1] = {true, true, false};
    CHECK(trigger.Update(false, false, slots, 200) == TriggerAction::None);

    slots[0] = {true, true, false};
    CHECK(trigger.Update(false, false, slots, 210) == TriggerAction::ToggleToDuo);

    slots[0] = {true, true, true};
    CHECK(trigger.Update(true, false, slots, 300) == TriggerAction::None);
    CHECK(trigger.Update(true, false, slots, 799) == TriggerAction::None);
    CHECK(trigger.Update(true, false, slots, 800) == TriggerAction::ToggleToSolo);
}

}  // namespace

int main() {
    TestF9EdgeToggle();
    TestController2StartEntersDuo();
    TestHoldBackStartExitsDuoAfter800Ms();
    TestDisabledControllerAndExplicitSlot();
    std::printf("All input_trigger_tests passed.\n");
    return 0;
}
