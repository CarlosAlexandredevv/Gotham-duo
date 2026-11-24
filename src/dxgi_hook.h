#pragma once

#include <Windows.h>

namespace gotham {

/// Initializes MinHook, starts DualOutput windows, and hooks DXGI factory/swapchain.
bool InitDxgiHook(HMODULE self_module);

/// Removes DXGI hooks, stops DualOutput, and uninitializes MinHook.
void ShutdownDxgiHook();

}  // namespace gotham
