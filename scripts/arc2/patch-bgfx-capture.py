"""Request a bounded consecutive sequence of bgfx GPU screenshots."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "build/arc2-testbeds/bgfx/examples/common/entry/entry.cpp"


def patch(old: str, new: str) -> None:
    content = SOURCE.read_text(encoding="utf-8-sig")
    if new in content:
        return
    if content.count(old) != 1:
        raise RuntimeError(f"Expected one bgfx capture anchor: {old!r}")
    SOURCE.write_text(content.replace(old, new), encoding="utf-8")


patch(', m_screenshotFrame(0)', ', m_screenshotFrame(0)\n\t\t\t, m_screenshotCount(1)')
patch('''\t\t\t\tconst char* frame = cmdLine.findOption("screenshot-frame");''', '''\t\t\t\tconst char* count = cmdLine.findOption("screenshot-count");
                if (NULL != count) {
                    bx::fromString(&m_screenshotCount, count);
                    m_screenshotCount = bx::clamp(m_screenshotCount, 1u, 256u);
                }
\t\t\t\tconst char* frame = cmdLine.findOption("screenshot-frame");''')
patch('''\t\t\tif (m_frame == m_screenshotFrame)
\t\t\t{
\t\t\t\tbgfx::requestScreenShot(BGFX_INVALID_HANDLE, m_screenshot);
\t\t\t}

\t\t\treturn m_frame < m_screenshotFrame + 3;''', '''\t\t\tif (m_frame >= m_screenshotFrame && m_frame < m_screenshotFrame + m_screenshotCount)
            {
                if (m_screenshotCount == 1) {
                    bgfx::requestScreenShot(BGFX_INVALID_HANDLE, m_screenshot);
                } else {
                    char path[bx::kMaxFilePath];
                    bx::snprintf(path, sizeof(path), "%s-%03u", m_screenshot,
                                 m_frame - m_screenshotFrame);
                    bgfx::requestScreenShot(BGFX_INVALID_HANDLE, path);
                }
            }

\t\t\treturn m_frame < m_screenshotFrame + m_screenshotCount + 3;''')
patch('''\t\tuint32_t m_screenshotFrame;
\t\tchar     m_screenshot[bx::kMaxFilePath];''', '''\t\tuint32_t m_screenshotFrame;
        uint32_t m_screenshotCount;
\t\tchar     m_screenshot[bx::kMaxFilePath];''')
print(f"bgfx capture count harness ready: {SOURCE}")
