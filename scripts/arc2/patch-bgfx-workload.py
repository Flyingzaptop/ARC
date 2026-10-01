"""Freeze bgfx drawstress draw count for identical benchmark arms."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "build/arc2-testbeds/bgfx/examples/17-drawstress/drawstress.cpp"


def patch(old: str, new: str) -> None:
    content = SOURCE.read_text(encoding="utf-8-sig")
    if new in content:
        return
    if content.count(old) != 1:
        raise RuntimeError(f"Expected one bgfx workload anchor: {old!r}")
    SOURCE.write_text(content.replace(old, new), encoding="utf-8")


patch('#include "common.h"', '#include "common.h"\n#include <cstdlib>')
patch('ImGui::SliderInt("Dim", &m_dim, 5, m_maxDim);', '''ImGui::SliderInt("Dim", &m_dim, 5, m_maxDim);
            // Benchmark selection only: ARC receives no scene or draw hints.
            if (const char* arc2_dim = std::getenv("ARC2_BGFX_DIM")) {
                m_autoAdjust = false;
                m_dim = bx::clamp(std::atoi(arc2_dim), 5, m_maxDim);
            }''')
patch('m_last = m_timeOffset = bx::getHPCounter();', '''m_last = m_timeOffset = bx::getHPCounter();
        m_arc2Frame = 0;''')
patch('float time = (float)( (now-m_timeOffset)/freq);', '''float time = (float)( (now-m_timeOffset)/freq);
            if (std::getenv("ARC2_BGFX_FIXED_TIME"))
                time = float(m_arc2Frame) / 60.0f; // Testbed quality route only.''')
patch('''\t\tif (!entry::processEvents(m_width, m_height, m_debug, m_reset, &m_mouseState) )
\t\t{''', '''\t\tif (!entry::processEvents(m_width, m_height, m_debug, m_reset, &m_mouseState) )
\t\t{
            ++m_arc2Frame;''')
patch('int64_t  m_timeOffset;', '''int64_t  m_timeOffset;
    uint32_t m_arc2Frame;''')
print(f"bgfx drawstress fixed-dimension harness ready: {SOURCE}")
