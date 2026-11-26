#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace gotham {

struct InputConfig {
    bool allow_controller = true;
    /// Controller slot index (0..3) for P2; -1 or any value outside 0..3 monitors slots 1..3.
    int p2_controller_index = -1;
    /// Duration (ms) Back + Start must be held together in Duo mode to exit to Solo.
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
    void SetConfig(InputConfig config) {
        if (config.p2_controller_index != config_.p2_controller_index) {
            combo_hold_start_ms_.fill(std::nullopt);
            wait_release_after_exit_.fill(false);
        }
        config_ = config;
    }
    const InputConfig& config() const { return config_; }

    /// Evaluates one input poll tick and returns any resulting mode transition.
    TriggerAction Update(bool is_duo,
                         bool f9_down,
                         const std::array<ControllerButtons, 4>& slots,
                         uint64_t now_ms);

private:
    void SyncStartAndResetCombos(const std::array<ControllerButtons, 4>& slots,
                                 bool clear_wait_release);

    InputConfig config_{};
    bool prev_f9_down_ = false;
    std::array<bool, 4> prev_start_down_{};
    std::array<std::optional<uint64_t>, 4> combo_hold_start_ms_{};
    std::array<bool, 4> wait_release_after_exit_{};
};

/// Polls live ToggleDuo key state (pass 0 to disable) and XInput controller slots 0..3.
void PollSystemInput(bool& out_f9_down,
                     std::array<ControllerButtons, 4>& out_slots,
                     int toggle_duo_vk = 0x78);

}  // namespace gotham
