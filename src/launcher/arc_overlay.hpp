#pragma once
#include <windows.h>
#include "json.hpp"
namespace arc::overlay {
void initialize(HINSTANCE instance,HWND owner);
void toggle();
bool enabled() noexcept;
void update(const nlohmann::json& automatic,const nlohmann::json& importance,DWORD pid);
void close();
}
