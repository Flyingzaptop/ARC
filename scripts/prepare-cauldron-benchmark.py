"""Apply the ARC test-host adapter to an extracted official FidelityFX SDK 1.1.4.

Does not change ARC core, assets, shaders or rendering quality. Backups make the
patch repeatable. Use a dedicated SDK extraction, never a developer checkout.
"""
import argparse
import json
from pathlib import Path
import shutil

parser = argparse.ArgumentParser()
parser.add_argument("sdk", type=Path)
args = parser.parse_args()
sdk = args.sdk.resolve()
repo = Path(__file__).resolve().parents[1]
framework = sdk / "framework/cauldron/framework"

def patch(relative, changes):
    path = sdk / relative
    backup = path.with_suffix(path.suffix + ".arc-original")
    if not backup.exists():
        shutil.copy2(path, backup)
    text = backup.read_text(encoding="utf-8-sig")
    for old, new in changes:
        if text.count(old) != 1:
            raise RuntimeError(f"Expected exactly one patch anchor in {relative}: {old[:80]}")
        text = text.replace(old, new)
    path.write_text(text, encoding="utf-8")

shutil.copy2(repo / "benchmarks/cauldron/arc_benchmark.h", framework / "inc/arc_benchmark.h")
patch("framework/cauldron/application/main.cpp", [
    ('#include "core/win/framework_win.h"', '#include "arc_benchmark.h"\n#include "core/win/framework_win.h"'),
    ('    // Create the sample and kick it off', '    arc_bench::initialize();\n    // Create the sample and kick it off'),
    ('    return RunFramework(&frameworkInstance);', '    const auto result=RunFramework(&frameworkInstance);\n    arc_bench::finish();return result;'),
])
patch("framework/cauldron/framework/src/core/framework.cpp", [
    ('#include <fstream>', '#include <fstream>\n#include "arc_benchmark.h"'),
    ('    void Framework::MainLoop()\n    {', '''    void Framework::MainLoop()
    {
        auto& bench=arc_bench::state();
        if(bench.enabled&&bench.tick>=bench.warmup+bench.frames)return;
        if(bench.enabled){
            const auto now=arc_bench::Clock::now();
            bench.period_ms=arc_bench::ms(now-bench.previous_start);bench.previous_start=now;bench.frame_start=now;
            bench.ready=m_pScene->IsReady()&&!m_pContentManager->IsCurrentlyLoading();
            for(auto* module:m_RenderModules)if(module->ModuleEnabled()&&!module->ModuleReady())bench.ready=false;
            if(bench.ready&&bench.tick==0)bench.ready_start=now;
            m_Config.LimitFPS=false;m_Config.GPULimitFPS=false;
        }'''),
    ('                compMgrIter->second->UpdateComponents(m_DeltaTime);', '                if(!bench.enabled||bench.ready)compMgrIter->second->UpdateComponents(m_DeltaTime);'),
    ('        if (m_pScene->IsReady())', '        if (m_pScene->IsReady()&&(!bench.enabled||bench.ready))'),
    ('        EndFrame();\n    }', '''        EndFrame();
        if(bench.enabled&&bench.ready){
            const auto frame_end=arc_bench::Clock::now();
            if(bench.tick>=bench.warmup){
                json row;row["frame"]=bench.tick-bench.warmup;row["pose"]=bench.tick%600;
                row["frame_ms"]=arc_bench::ms(frame_end-bench.previous_end);row["loop_ms"]=arc_bench::ms(arc_bench::Clock::now()-bench.frame_start);
                row["present_ms"]=bench.present_ms;row["submit_ms"]=bench.submit_ms;
                row["swapchain_wait_ms"]=bench.wait_ms;row["allocator_wait_ms"]=bench.allocator_ms;
                for(const auto& timing:m_pProfiler->GetCPUTimings())row["cpu_ms"][WStringToString(timing.Label)]=double(timing.GetDuration().count())/1000000.;
                for(const auto& timing:m_pProfiler->GetGPUTimings())row["gpu_ms"][WStringToString(timing.Label)]=double(timing.GetDuration().count())/1000000.;
                row["gpu_span_ms"]=double(m_pProfiler->GetGPUFrameTicks())/1000000.;
                if(!m_pProfiler->GetGPUTimings().empty()){
                    row["gpu_begin_ns"]=m_pProfiler->GetGPUTimings().front().StartTime.count();
                    row["gpu_end_ns"]=m_pProfiler->GetGPUTimings().back().EndTime.count();
                }
                bench.rows<<row.dump()<<'\\n';
            }
            bench.previous_end=frame_end;
            ++bench.tick;
            if(bench.tick>=bench.warmup+bench.frames||arc_bench::ms(arc_bench::Clock::now()-bench.ready_start)>55000){
                m_StopTime=std::chrono::steady_clock::now();PostQuitMessage(0);
            }
        }
    }'''),
    ('        m_pSwapChain->WaitForSwapChain();', '''        const auto swap_wait_start=arc_bench::Clock::now();
        m_pSwapChain->WaitForSwapChain();
        arc_bench::state().wait_ms=arc_bench::ms(arc_bench::Clock::now()-swap_wait_start);'''),
    ('        m_pDeviceCmdListForFrame = m_pDevice->BeginFrame();', '''        const auto allocator_start=arc_bench::Clock::now();
        m_pDeviceCmdListForFrame = m_pDevice->BeginFrame();
        arc_bench::state().allocator_ms=arc_bench::ms(arc_bench::Clock::now()-allocator_start);'''),
    ('        m_LastFrameTime = now;', '        m_LastFrameTime = now;\n        if(arc_bench::state().enabled)m_DeltaTime=arc_bench::state().ready?1.0/60.0:0.0;'),
    ('        m_pDevice->SubmitCmdListBatch(m_vecCmdListsForFrame, CommandQueue::Graphics, true);', '''        const auto submit_start=arc_bench::Clock::now();
        m_pDevice->SubmitCmdListBatch(m_vecCmdListsForFrame, CommandQueue::Graphics, true);
        arc_bench::state().submit_ms=arc_bench::ms(arc_bench::Clock::now()-submit_start);'''),
    ('        m_pSwapChain->Present();', '''        const auto present_start=arc_bench::Clock::now();
        m_pSwapChain->Present();
        arc_bench::state().present_ms=arc_bench::ms(arc_bench::Clock::now()-present_start);'''),
    ('        if (m_PerfFrameCount == m_Config.BenchmarkFrameDuration)', '        if (!arc_bench::state().enabled&&m_PerfFrameCount == m_Config.BenchmarkFrameDuration)'),
    ('<< L".jpg";', '<< (arc_bench::state().enabled?L".png":L".jpg");'),
])
patch("framework/cauldron/framework/src/core/components/cameracomponent.cpp", [
    ('#include "core/components/cameracomponent.h"', '#include "core/components/cameracomponent.h"\n#include "arc_benchmark.h"'),
    ('        if (m_SkipUpdate)', '        if (m_SkipUpdate&&!arc_bench::state().enabled)'),
    ('            // if (animated)', '''            if(arc_bench::state().enabled){
                const float phase=float(arc_bench::state().tick%600)*6.28318530718f/600.f;
                const Vec3 right=m_ResetMatrix.getCol0().getXYZ(),forward=-m_ResetMatrix.getCol2().getXYZ();
                const Vec3 eye=m_ResetMatrix.getTranslation()+right*(sin(phase)*0.8f)+forward*((1-cos(phase))*0.65f);
                const Vec3 target=eye+forward*3.f+right*(sin(phase*2.f)*0.5f);
                LookAt(Vec4(eye,1.f),Vec4(target,1.f));UpdateMatrices();return;
            }
            // if (animated)'''),
])
patch("framework/cauldron/framework/src/render/dx12/swapchain_dx12.cpp", [
    ('        CauldronThrowOnFail(GetDevice()->GetImpl()->DX12CmdQueue(CommandQueue::Graphics)->Signal(pFence, 1));', ''),
    ('        GetDevice()->GetImpl()->DX12CmdQueue(CommandQueue::Graphics)->ExecuteCommandLists(1, CmdListList);',
     '        GetDevice()->GetImpl()->DX12CmdQueue(CommandQueue::Graphics)->ExecuteCommandLists(1, CmdListList);\n        CauldronThrowOnFail(GetDevice()->GetImpl()->DX12CmdQueue(CommandQueue::Graphics)->Signal(pFence, 1));'),
    ('        stbi_write_jpg(WStringToString(filePath.c_str()).c_str(), (int)fromDesc.Width, (int)fromDesc.Height, 4, pTimingsBuffer, 100);', '''        if(filePath.extension()==L".png")
            stbi_write_png(WStringToString(filePath.c_str()).c_str(),(int)fromDesc.Width,(int)fromDesc.Height,4,pTimingsBuffer,(int)fromDesc.Width*4);
        else stbi_write_jpg(WStringToString(filePath.c_str()).c_str(), (int)fromDesc.Width, (int)fromDesc.Height, 4, pTimingsBuffer, 100);'''),
])
patch("framework/cauldron/framework/src/render/rendermodules/ui/uirendermodule.cpp", [
    ('#include "render/rendermodules/ui/uirendermodule.h"', '#include "render/rendermodules/ui/uirendermodule.h"\n#include "arc_benchmark.h"'),
    ('    void UIRenderModule::Execute(double deltaTime, CommandList* pCmdList)\n    {', '    void UIRenderModule::Execute(double deltaTime, CommandList* pCmdList)\n    {\n        if(arc_bench::state().enabled)return;'),
])
patch("framework/cauldron/framework/src/render/rendermodules/fpslimiter/fpslimiterrendermodule.cpp", [
    ('#include "fpslimiterrendermodule.h"', '#include "fpslimiterrendermodule.h"\n#include "arc_benchmark.h"'),
    ('    if (!m_LimitFPS)', '    if (arc_bench::state().enabled||!m_LimitFPS)'),
])
config = sdk / "framework/cauldron/framework/config/cauldronconfig.json"
if not config.exists():
    config = next(framework.rglob("cauldronconfig.json"))
data=json.loads(config.read_text())
data["Cauldron"]["Presentation"].update(Vsync=False,Width=1920,Height=1080)
data["Cauldron"]["FPSLimiter"].update(Enable=False,UseGPULimiter=False)
config.write_text(json.dumps(data,indent=2))
print(f"Prepared deterministic benchmark host at {sdk}; build FFX_BRIXELIZER_GI ReleaseDX12")
