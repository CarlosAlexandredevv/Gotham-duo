#pragma once

namespace gotham {

/// Loads the real System32\version.dll and binds all forwarded export pointers.
bool InitVersionProxy();

/// Unloads the System32\version.dll handle if loaded.
void ShutdownVersionProxy();

}  // namespace gotham
