#pragma once
namespace arc::dx12::children {
bool configure(const wchar_t* config) noexcept;
bool install() noexcept;
void stop() noexcept;
}
