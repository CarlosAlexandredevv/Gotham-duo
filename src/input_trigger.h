#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace gotham {

struct InputConfig {
    bool allow_controller = true;
    int p2_controller_index = -1;
    uint32_t hold_back_start_ms = 800;
};

struct ControllerButtons {
    bool connected = false;
    bool start = false;
    bool back = false;
};

enum class TriggerAction {
    None,
    ToggleToDuo,
    ToggleToSolo,
};

/// Pure state machine translating F9 and controller inputs into Solo/Duo mode transitions.
class InputTrigger {
public:
    void SetConfig(const InputConfig& config) { config_ = config; }
    const InputConfig& config() const { return config_; }

    /// Evaluates one input poll tick and returns any resulting mode transition.
    TriggerAction Update(bool is_duo,
                         bool f9_down,
                         const std::array<ControllerButtons, 4>& slots,
                         uint64_t now_ms);

private:
    InputConfig config_{};
    bool prev_f9_down_ = false;
    std::array<bool, 4> prev_start_down_{};
    std::array<std::optional<uint64_t>, 4> combo_hold_start_ms_{};
    std::array<bool, 4> wait_release_after_exit_{};
};

/// Polls live F9 key state and XInput controller slots 0..3.
void PollSystemInput(bool& out_f9_down, std::array<ControllerButtons, 4>& out_slots);

}  // namespace gotham
