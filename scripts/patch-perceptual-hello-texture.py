"""Apply a reproducible adapter overlay to a CLEAN pinned Microsoft sample."""
import argparse
import shutil
import subprocess
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("upstream", type=Path)
args = p.parse_args()
root = args.upstream.resolve()
sha = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
if sha != "213dd4fd4918ea009dd8f35adee1aff1f2ecaba4":
    raise SystemExit("Wrong pinned Microsoft revision")
if subprocess.check_output(["git", "-C", str(root), "diff", "--name-only"], text=True).strip():
    raise SystemExit("Use a clean isolated worktree; existing modifications are preserved")
sample = root / "Samples/Desktop/D3D12HelloWorld/src/HelloTexture"
repo = Path(__file__).resolve().parents[1]
shutil.copyfile(repo / "integrations/directx-hello-texture/ArcPerceptualHost.h", sample / "ArcPerceptualHost.h")

def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Nonunique/missing patch anchor: {old[:100]}")
    return text.replace(old, new)

h = (sample / "D3D12HelloTexture.h").read_text()
h = replace(h, '#include "DXSample.h"', '#include "DXSample.h"\n#include "ArcPerceptualHost.h"')
h = replace(h, '    ComPtr<ID3D12Resource> m_texture;', '    ComPtr<ID3D12Resource> m_texture;\n    std::unique_ptr<ArcPerceptualHost> m_perceptual;\n    bool m_probeStarted = false;')
(sample / "D3D12HelloTexture.h").write_text(h)
cpp = (sample / "D3D12HelloTexture.cpp").read_text()
cpp = cpp.replace('u8".\\\\D3D12\\\\"', '".\\\\D3D12\\\\"')
cpp = replace(cpp, '    LoadAssets();', '''    LoadAssets();
    wchar_t output[32768]{};
    if (!GetEnvironmentVariableW(L"ARC_PERCEPTUAL_OUTPUT", output, 32768)) throw std::runtime_error("ARC_PERCEPTUAL_OUTPUT required");
    m_perceptual = std::make_unique<ArcPerceptualHost>(m_device.Get(), m_commandQueue.Get(), m_texture.Get(), m_srvHeap->GetCPUDescriptorHandleForHeapStart(), [this]() {
        const auto index = m_frameIndex; OnRender(); return m_renderTargets[index].Get();
    }, output);''')
cpp = replace(cpp, 'textureDesc.MipLevels = 1;', 'textureDesc.MipLevels = 2;')
cpp = replace(cpp, 'GetRequiredIntermediateSize(m_texture.Get(), 0, 1)', 'GetRequiredIntermediateSize(m_texture.Get(), 0, 2)')
cpp = replace(cpp, 'srvDesc.Texture2D.MipLevels = 1;', 'srvDesc.Texture2D.MipLevels = 2;')
cpp = replace(cpp, '        UpdateSubresources(m_commandList.Get(), m_texture.Get(), textureUploadHeap.Get(), 0, 0, 1, &textureData);', '''        std::vector<UINT8> lower(TextureWidth * TextureHeight * TexturePixelSize / 4);
        for (UINT y = 0; y < TextureHeight/2; ++y) for (UINT x = 0; x < TextureWidth/2; ++x) for (UINT c = 0; c < 4; ++c) {
            UINT sum = 0;
            for (UINT dy=0; dy<2; ++dy) for (UINT dx=0; dx<2; ++dx) sum += texture[((2*y+dy)*TextureWidth+2*x+dx)*4+c];
            lower[(y*(TextureWidth/2)+x)*4+c] = UINT8(sum/4);
        }
        D3D12_SUBRESOURCE_DATA levels[2] = {textureData, {lower.data(), TextureWidth * TexturePixelSize / 2, TextureWidth * TextureHeight * TexturePixelSize / 4}};
        UpdateSubresources(m_commandList.Get(), m_texture.Get(), textureUploadHeap.Get(), 0, 0, 2, levels);''')
cpp = replace(cpp, 'void D3D12HelloTexture::OnRender()\n{', '''void D3D12HelloTexture::OnRender()
{
    if (!m_probeStarted) {
        m_probeStarted = true;
        try { PostQuitMessage(m_perceptual->run()); }
        catch (const std::exception& e) { std::ofstream("perceptual-error.txt") << e.what(); PostQuitMessage(9); }
        return;
    }''')
cpp = replace(cpp, '    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);', '''    m_perceptual->begin_frame(m_commandList.Get());
    m_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);''')
last_barrier = '    m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_renderTargets[m_frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));'
cpp = replace(cpp, last_barrier, last_barrier+'\n    m_perceptual->end_frame(m_commandList.Get());')
(sample / "D3D12HelloTexture.cpp").write_text(cpp)
main = (sample / "Main.cpp").read_text()
main = replace(main, '    return Win32Application::Run(&sample, hInstance, nCmdShow);', '''    try { return Win32Application::Run(&sample, hInstance, nCmdShow); }
    catch (const std::exception& e) { std::ofstream("perceptual-error.txt") << e.what(); return 9; }''')
(sample / "Main.cpp").write_text(main)
print(f"Applied portable perceptual adapter to {sha}")
