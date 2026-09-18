#pragma once

#include "stdafx.h"

namespace arc_wicked {

void HarnessUpdate(
    wi::RenderPath3D& render_path,
    wi::gui::ComboBox& test_selector,
    std::uint32_t width,
    std::uint32_t height) noexcept;

void ForceFullQuality() noexcept;
bool Finished() noexcept;

} // namespace arc_wicked
