#include "input_trigger.h"

#include <Windows.h>
#include <Xinput.h>

namespace gotham {

TriggerAction InputTrigger::Update(bool is_duo,
                                   bool f9_down,
                                   const std::array<ControllerButtons, 4>& slots,
                                   uint64_t now_ms) {
    const bool f9_rising = f9_down && !prev_f9_down_;
    prev_f9_down_ = f9_down;
    if (f9_rising) {
        combo_hold_start_ms_.fill(std::nullopt);
        for (size_t i = 0; i < slots.size(); ++i) {
            prev_start_down_[i] = slots[i].connected && slots[i].start;
        }
        return is_duo ? TriggerAction::ToggleToSolo : TriggerAction::ToggleToDuo;
    }

    if (!config_.allow_controller) {
        return TriggerAction::None;
    }

    const bool explicit_slot =
        config_.p2_controller_index >= 0 && config_.p2_controller_index < 4;
    const int begin_slot = explicit_slot ? config_.p2_controller_index : 1;
    const int end_slot = explicit_slot ? (config_.p2_controller_index + 1) : 4;

    TriggerAction action = TriggerAction::None;
    for (int i = begin_slot; i < end_slot; ++i) {
        const ControllerButtons& s = slots[i];
        if (!s.connected) {
            prev_start_down_[i] = false;
            combo_hold_start_ms_[i].reset();
            wait_release_after_exit_[i] = false;
            continue;
        }

        if (wait_release_after_exit_[i]) {
            if (!s.start && !s.back) {
                wait_release_after_exit_[i] = false;
            }
            prev_start_down_[i] = s.start;
            combo_hold_start_ms_[i].reset();
            continue;
        }

        const bool was_start_down = prev_start_down_[i];
        prev_start_down_[i] = s.start;

        if (!is_duo) {
            combo_hold_start_ms_[i].reset();
            if (s.start && !was_start_down && action == TriggerAction::None) {
                action = TriggerAction::ToggleToDuo;
            }
            continue;
        }

        if (s.start && s.back) {
            if (!combo_hold_start_ms_[i].has_value()) {
                combo_hold_start_ms_[i] = now_ms;
            }
            if (now_ms - *combo_hold_start_ms_[i] >= config_.hold_back_start_ms) {
                combo_hold_start_ms_[i].reset();
                wait_release_after_exit_[i] = true;
                if (action == TriggerAction::None) {
                    action = TriggerAction::ToggleToSolo;
                }
            }
        } else {
            combo_hold_start_ms_[i].reset();
        }
    }

    return action;
}

void PollSystemInput(bool& out_f9_down, std::array<ControllerButtons, 4>& out_slots) {
    out_f9_down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;

    using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    static const XInputGetStateFn xinput_get_state = []() -> XInputGetStateFn {
        HMODULE mod = LoadLibraryW(L"xinput1_4.dll");
        if (!mod) {
            mod = LoadLibraryW(L"xinput9_1_0.dll");
        }
        return mod ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(mod, "XInputGetState"))
                   : nullptr;
    }();

    for (DWORD i = 0; i < 4; ++i) {
        XINPUT_STATE state{};
        if (xinput_get_state && xinput_get_state(i, &state) == ERROR_SUCCESS) {
            out_slots[i].connected = true;
            out_slots[i].start = (state.Gamepad.wButtons & XINPUT_GAMEPAD_START) != 0;
            out_slots[i].back = (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) != 0;
        } else {
            out_slots[i] = ControllerButtons{};
        }
    }
}

}  // namespace gotham
