"""Deterministic Diligent sample update clock for moving-scene image checks."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "build/arc2-testbeds/DiligentEngine/DiligentSamples/SampleBase/src/SampleApp.cpp"
content = SOURCE.read_text(encoding="utf-8-sig")
anchor = 'void SampleApp::Update(double CurrTime, double ElapsedTime)\n{\n    m_CurrentTime = CurrTime;'
replacement = '''void SampleApp::Update(double CurrTime, double ElapsedTime)
{
    if (std::getenv("ARC2_DILIGENT_FIXED_DT")) {
        static unsigned long long arc2_frame = 0;
        CurrTime = double(arc2_frame++) / 60.0;
        ElapsedTime = 1.0 / 60.0;
    }
    m_CurrentTime = CurrTime;'''
if replacement not in content:
    if content.count(anchor) != 1:
        raise RuntimeError("Diligent SampleApp Update anchor changed")
    SOURCE.write_text(content.replace(anchor, replacement), encoding="utf-8")
print(f"Diligent fixed-step capture route ready: {SOURCE}")
