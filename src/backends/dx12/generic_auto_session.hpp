#pragma once
#include <windows.h>
#include <iosfwd>

namespace arc::dx12::autotune {
bool start(const wchar_t* config_path) noexcept;
void stop() noexcept;
bool target(double fps) noexcept;
bool active() noexcept;
void present(void* swapchain,HRESULT result,UINT flags) noexcept;
void snapshot(std::ostream&);
}
