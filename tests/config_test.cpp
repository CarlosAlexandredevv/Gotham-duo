#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "CHECK failed (%s:%d): %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                  \
        }                                                                                  \
    } while (false)

namespace {

using gotham::AppConfig;
using gotham::kDefaultReapplyResizeVk;
using gotham::kDefaultToggleDuoVk;
using gotham::kDefaultToggleMirrorVk;
using gotham::LoadConfigFromFile;
using gotham::ParseConfig;
using gotham::ParseVirtualKey;

void TestDefaultsAndMissingFile() {
    const AppConfig cfg = LoadConfigFromFile(L"nonexistent_gothamduo_test_file.ini");
    CHECK(cfg.monitors.player1 == -1);
    CHECK(cfg.monitors.player2 == -1);
    CHECK(cfg.input.allow_controller);
    CHECK(cfg.input.p2_controller_index == -1);
    CHECK(cfg.input.hold_back_start_ms == 800);
    CHECK(cfg.hotkeys.toggle_duo_vk == kDefaultToggleDuoVk);
    CHECK(cfg.hotkeys.toggle_mirror_vk == kDefaultToggleMirrorVk);
    CHECK(cfg.hotkeys.reapply_resize_vk == kDefaultReapplyResizeVk);
    CHECK(cfg.mirror.auto_detect);
    CHECK(cfg.mirror.enter_mirror_ms == 200);
    CHECK(cfg.mirror.exit_mirror_ms == 100);
    CHECK(cfg.display.target_width == 0);
    CHECK(cfg.display.target_height == 0);
    CHECK(cfg.display.solo_width == 0);
    CHECK(cfg.display.solo_height == 0);
    CHECK(cfg.logging.enabled);
    CHECK(!cfg.logging.verbose);
    CHECK(cfg.performance.suppress_game_present);
    CHECK(cfg.warnings.empty());
}

void TestVirtualKeyParsing() {
    CHECK(ParseVirtualKey("F1") == 0x70);
    CHECK(ParseVirtualKey("f9") == 0x78);
    CHECK(ParseVirtualKey("F10") == 0x79);
    CHECK(ParseVirtualKey("F24") == 0x87);
    CHECK(ParseVirtualKey("a") == 'A');
    CHECK(ParseVirtualKey("Z") == 'Z');
    CHECK(ParseVirtualKey("D0") == '0');
    CHECK(ParseVirtualKey("Num5") == 0x65);
    CHECK(ParseVirtualKey("Insert") == 0x2D);
    CHECK(ParseVirtualKey("Pause") == 0x13);
    CHECK(ParseVirtualKey("0x7B") == 0x7B);

    // Explicit disable tokens map to 0.
    CHECK(ParseVirtualKey("None") == 0);
    CHECK(ParseVirtualKey("disabled") == 0);
    CHECK(ParseVirtualKey("off") == 0);
    CHECK(ParseVirtualKey("0") == 0);
    CHECK(ParseVirtualKey("   ") == 0);

    // Invalid key names fail cleanly.
    CHECK(!ParseVirtualKey("F0").has_value());
    CHECK(!ParseVirtualKey("F25").has_value());
    CHECK(!ParseVirtualKey("0x00").has_value());
    CHECK(!ParseVirtualKey("0x100").has_value());
    CHECK(!ParseVirtualKey("NotAKey").has_value());
}

void TestValidCustomConfig() {
    const std::string ini =
        "\xEF\xBB\xBF"
        "; Comment line\r\n"
        "[Monitors]\r\n"
        "Player1 = 1\r\n"
        "Player2 = 0 ; inline comment\r\n"
        "\r\n"
        "[Input]\r\n"
        "AllowControllerToggle=false\r\n"
        "P2ControllerIndex=2\r\n"
        "HoldBackStartMs=1200\r\n"
        "\r\n"
        "[Hotkeys]\r\n"
        "ToggleDuoKey=F8\r\n"
        "ToggleMirrorKey=None\r\n"
        "ReapplyResizeKey=0x2D\r\n"
        "\r\n"
        "[MenuMirror]\r\n"
        "AutoDetect=0\r\n"
        "EnterMirrorMs=350\r\n"
        "ExitMirrorMs=0\r\n"
        "\r\n"
        "[Display]\r\n"
        "TargetWidth=5120\r\n"
        "TargetHeight=1440\r\n"
        "SoloWidth=2560\r\n"
        "SoloHeight=1440\r\n"
        "\r\n"
        "[Performance]\r\n"
        "SuppressGamePresent=0\r\n"
        "\r\n"
        "[Logging]\r\n"
        "Enabled=0\r\n"
        "Verbose=true\r\n";

    const AppConfig cfg = ParseConfig(ini);
    CHECK(cfg.warnings.empty());
    CHECK(!cfg.performance.suppress_game_present);
    CHECK(cfg.monitors.player1 == 1);
    CHECK(cfg.monitors.player2 == 0);
    CHECK(!cfg.input.allow_controller);
    CHECK(cfg.input.p2_controller_index == 2);
    CHECK(cfg.input.hold_back_start_ms == 1200);
    CHECK(cfg.hotkeys.toggle_duo_vk == 0x77);
    CHECK(cfg.hotkeys.toggle_mirror_vk == 0);
    CHECK(cfg.hotkeys.reapply_resize_vk == 0x2D);
    CHECK(!cfg.mirror.auto_detect);
    CHECK(cfg.mirror.enter_mirror_ms == 350);
    CHECK(cfg.mirror.exit_mirror_ms == 0);
    CHECK(cfg.display.target_width == 5120);
    CHECK(cfg.display.target_height == 1440);
    CHECK(cfg.display.solo_width == 2560);
    CHECK(cfg.display.solo_height == 1440);
    CHECK(!cfg.logging.enabled);
    CHECK(cfg.logging.verbose);
}

void TestMalformedEntriesFallbackWithWarnings() {
    const std::string ini =
        "NotAKeyValueLine\n"
        "[Monitors]\n"
        "Player1=-5\n"
        "[Input]\n"
        "AllowControllerToggle=maybe\n"
        "P2ControllerIndex=9\n"
        "HoldBackStartMs=0\n"
        "[Hotkeys]\n"
        "ToggleDuoKey=InvalidKey\n"
        "ToggleMirrorKey=F99\n"
        "[Display]\n"
        "TargetWidth=-100\n"
        "[Performance]\n"
        "SuppressGamePresent=maybe\n"
        "[Logging]\n"
        "Enabled=nope\n";

    const AppConfig cfg = ParseConfig(ini);
    // Every malformed field must fall back to its default and emit a warning.
    CHECK(cfg.monitors.player1 == -1);
    CHECK(cfg.input.allow_controller);
    CHECK(cfg.input.p2_controller_index == -1);
    CHECK(cfg.input.hold_back_start_ms == 800);
    CHECK(cfg.hotkeys.toggle_duo_vk == kDefaultToggleDuoVk);
    CHECK(cfg.hotkeys.toggle_mirror_vk == kDefaultToggleMirrorVk);
    CHECK(cfg.display.target_width == 0);
    CHECK(cfg.logging.enabled);
    CHECK(cfg.performance.suppress_game_present);
    CHECK(cfg.warnings.size() == 10);
}

}  // namespace

int main() {
    TestDefaultsAndMissingFile();
    TestVirtualKeyParsing();
    TestValidCustomConfig();
    TestMalformedEntriesFallbackWithWarnings();
    std::printf("All config_tests passed.\n");
    return 0;
}
