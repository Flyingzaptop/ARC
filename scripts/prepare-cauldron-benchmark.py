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
shutil.copy2(repo / "include/arc/diagnostic_timeline.hpp", framework / "inc/arc_timeline.h")
shutil.copy2(repo / "benchmarks/cauldron/arc_async_writer.h", framework / "inc/arc_async_writer.h")
patch("framework/cauldron/application/main.cpp", [
    ('#include "core/win/framework_win.h"', '#include "arc_benchmark.h"\n#include "core/win/framework_win.h"'),
    ('    // Create the sample and kick it off', '    arc_bench::initialize();\n    // Create the sample and kick it off'),
    ('    return RunFramework(&frameworkInstance);', '    const auto result=RunFramework(&frameworkInstance);\n    arc_bench::finish();return result;'),
])
patch("framework/cauldron/framework/src/core/framework.cpp", [
    ('#include <fstream>', '#include <fstream>\n#include "arc_benchmark.h"'),
    ('    void Framework::MainLoop()\n    {', '''    void Framework::MainLoop()
    {
        arc::timeline::Scope loop_trace("main_loop");
        auto& bench=arc_bench::state();
        arc::timeline::recorder().frame=bench.tick;
        if(bench.enabled&&bench.finished)return;
        if(bench.enabled){
            const auto now=arc_bench::Clock::now();
            bench.period_ms=arc_bench::ms(now-bench.previous_start);bench.previous_start=now;bench.frame_start=now;
            bench.ready=m_pScene->IsReady()&&!m_pContentManager->IsCurrentlyLoading();
            for(auto* module:m_RenderModules)if(module->ModuleEnabled()&&!module->ModuleReady())bench.ready=false;
            if(bench.ready&&bench.tick==0)bench.ready_start=now;
            m_Config.LimitFPS=false;m_Config.GPULimitFPS=false;arc_bench::before_frame();
        }'''),
    ('                compMgrIter->second->UpdateComponents(m_DeltaTime);', '                if(!bench.enabled||bench.ready)compMgrIter->second->UpdateComponents(m_DeltaTime);'),
    ('        if (m_pScene->IsReady())', '        if (m_pScene->IsReady()&&(!bench.enabled||bench.ready))'),
    ('        EndFrame();\n    }', '''        EndFrame();
        if(bench.enabled&&bench.ready){
            const auto frame_end=arc_bench::Clock::now();
            const bool measured=bench.timed?bench.measuring:bench.tick>=bench.warmup;
            {
                arc::timeline::Scope telemetry_trace("frame_telemetry");
                json row;row["cpu_profiler_frame"]=bench.tick?json(bench.tick-1):json(nullptr);row["phase"]=measured?"measurement":"initialization";row["ready_elapsed_ms"]=arc_bench::ms(frame_end-bench.ready_start);row["frame"]=measured?bench.measured_frames++:bench.tick;row["pose"]=bench.tick%600;row["scene_frame"]=bench.tick;
                row["measurement_elapsed_ms"]=(bench.timed&&bench.measuring)?arc_bench::ms(frame_end-bench.measurement_start):0;
                DWORD foreground_pid{};GetWindowThreadProcessId(GetForegroundWindow(),&foreground_pid);row["foreground"]=foreground_pid==GetCurrentProcessId();
                row["frame_ms"]=arc_bench::ms(frame_end-(bench.tick?bench.previous_end:bench.frame_start));row["loop_ms"]=arc_bench::ms(arc_bench::Clock::now()-bench.frame_start);
                row["present_ms"]=bench.present_ms;row["submit_ms"]=bench.submit_ms;
                row["swapchain_wait_ms"]=bench.wait_ms;row["allocator_wait_ms"]=bench.allocator_ms;
                for(const auto& timing:m_pProfiler->GetCPUTimings())row["cpu_ms"][WStringToString(timing.Label)]=double(timing.GetDuration().count())/1000000.;
                for(const auto& timing:m_pProfiler->GetGPUTimings()){
                    const auto label=WStringToString(timing.Label);row["gpu_ms"][label]=double(timing.GetDuration().count())/1000000.;
                    row["gpu_intervals"][label]={{"begin_ns",timing.StartTime.count()},{"end_ns",timing.EndTime.count()}};
                }
                row["gpu_span_ms"]=double(m_pProfiler->GetGPUFrameTicks())/1000000.;
                if(!m_pProfiler->GetGPUTimings().empty()){
                    row["gpu_begin_ns"]=m_pProfiler->GetGPUTimings().front().StartTime.count();
                    row["gpu_end_ns"]=m_pProfiler->GetGPUTimings().back().EndTime.count();
                }
                auto* destination=measured?&bench.rows:&bench.initialization_rows;
                bench.writer->push([row=std::move(row),destination](std::ostream&){arc::timeline::Scope write_trace("frame_json_serialize_write");auto& output=*destination;output<<row.dump()<<'\\n';if(!output)throw std::runtime_error("Benchmark telemetry write failed");});
            }
            bench.previous_end=frame_end;
            if(arc_bench::oracle_due()){
                m_pSwapChain->DumpSwapChainToFile(bench.output+L"/oracle-"+std::to_wstring(bench.tick)+L".png");
                if(bench.snapshot){bench.snapshot(nullptr);CopyFileW((bench.output+L"/arc.json").c_str(),(bench.output+L"/oracle-"+std::to_wstring(bench.tick)+L".arc.json").c_str(),TRUE);}
                std::ofstream oracle(bench.output+L"/oracle.jsonl",std::ios::app);oracle<<json({{"scene_frame",bench.tick},{"pose",bench.tick%600},{"elapsed_ms",arc_bench::ms(frame_end-bench.ready_start)}}).dump()<<'\\n';
            }
            ++bench.tick;
            const bool done=bench.timed?(bench.measuring&&arc_bench::ms(frame_end-bench.measurement_start)>=bench.measurement_seconds*1000):(bench.tick>=bench.warmup+bench.frames||arc_bench::ms(frame_end-bench.ready_start)>(bench.oracle_poses?180000:55000));
            if(done){bench.finished=true;
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
                if(CameraComponent::s_pSetJitterCallback)s_pSetJitterCallback(m_jitterValues);
                const unsigned pose=arc_bench::state().tick%600;
                const float phase=float(pose)*6.28318530718f/600.f;
                if(arc_bench::state().holdout){
                    const float t=float(pose<180?pose:pose<240?180:pose-60)*6.28318530718f/540.f;
                    const Vec3 right=m_ResetMatrix.getCol0().getXYZ(),forward=-m_ResetMatrix.getCol2().getXYZ();
                    const Vec3 eye=m_ResetMatrix.getTranslation()+right*(sin(t)*1.1f)+forward*((1-cos(t))*.8f);
                    const float turn=pose>=360&&pose<400?.9f:0.f;
                    const Vec3 target=eye+forward*3.f+right*(sin(t*3.f)*.7f+turn);
                    LookAt(Vec4(eye,1.f),Vec4(target,1.f));UpdateMatrices();return;
                }
                const Vec3 right=m_ResetMatrix.getCol0().getXYZ(),forward=-m_ResetMatrix.getCol2().getXYZ();
                const Vec3 eye=m_ResetMatrix.getTranslation()+right*(sin(phase)*0.8f)+forward*((1-cos(phase))*0.65f);
                const Vec3 target=eye+forward*3.f+right*(sin(phase*2.f)*0.5f);
                LookAt(Vec4(eye,1.f),Vec4(target,1.f));UpdateMatrices();return;
            }
            // if (animated)'''),
])
patch("framework/cauldron/framework/src/core/components/animationcomponent.cpp", [
    ('#include "core/components/animationcomponent.h"', '#include "core/components/animationcomponent.h"\n#include "arc_benchmark.h"'),
    ('        time += deltaTime;', '''        if(arc_bench::state().enabled){const double phase=double(arc_bench::state().tick%600)*6.283185307179586/600.;time=30.+2.5*sin(phase+(arc_bench::state().holdout?1.4:.7));}
        else time += deltaTime;'''),
])
patch("framework/cauldron/framework/inc/core/components/lightcomponent.h", [
    ('        LightComponentData* m_pData;', '        LightComponentData* m_pData;\n        bool m_ArcIntensityCaptured = false;\n        float m_ArcBaseIntensity = 0.f;'),
])
patch("framework/cauldron/framework/src/core/components/lightcomponent.cpp", [
    ('#include "core/components/lightcomponent.h"', '#include "core/components/lightcomponent.h"\n#include "arc_benchmark.h"'),
    ('            // Do light updates (todo - animate lights if needed)', '''            if(arc_bench::state().enabled&&arc_bench::state().holdout){
                if(!m_ArcIntensityCaptured){m_ArcBaseIntensity=m_pData->Intensity;m_ArcIntensityCaptured=true;}
                const float phase=float(arc_bench::state().tick%600)*6.28318530718f/600.f;
                m_pData->Intensity=m_ArcBaseIntensity*(1.f+.35f*sin(phase*2.f));
            }
            // Do light updates (todo - animate lights if needed)'''),
])
patch("framework/cauldron/framework/src/render/dx12/swapchain_dx12.cpp", [
    ('        pCmdList->GetImpl()->DX12CmdList()->CopyTextureRegion(&copyDest, 0, 0, 0, &copySrc, nullptr);',
     '        pCmdList->GetImpl()->DX12CmdList()->CopyTextureRegion(&copyDest, 0, 0, 0, &copySrc, nullptr);\n        Barrier restore = Barrier::Transition(m_pRenderTarget->GetCurrentResource(), ResourceState::CopySource, ResourceState::Present);\n        ResourceBarrier(pCmdList, 1, &restore);'),
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
patch("framework/cauldron/framework/src/core/win/framework_win.cpp", [
    ('#include "core/win/framework_win.h"', '#include "core/win/framework_win.h"\n#include "arc_benchmark.h"'),
    ('            GetDevice()->UpdateAntiLag2();', '            arc::timeline::Scope outer_trace("outer_loop");\n            {arc::timeline::Scope trace("anti_lag");GetDevice()->UpdateAntiLag2();}'),
    ('            while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))', '            {arc::timeline::Scope trace("message_pump");\n            while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))'),
    ('            // Only update if we', '            }\n            // Only update if we'),
])
patch("framework/cauldron/framework/src/core/win/inputmanager_win.cpp", [
    ('#include "core/win/inputmanager_win.h"', '#include "core/win/inputmanager_win.h"\n#include "arc_benchmark.h"'),
    ('        if (XInputGetState(0, &controllerState) == ERROR_SUCCESS)', '        const auto controllerResult=[&]{arc::timeline::Scope trace("xinput_get_state");return XInputGetState(0, &controllerState);}();\n        if (controllerResult == ERROR_SUCCESS)'),
])
config = sdk / "framework/cauldron/framework/config/cauldronconfig.json"
if not config.exists():
    config = next(framework.rglob("cauldronconfig.json"))
data=json.loads(config.read_text())
data["Cauldron"]["Presentation"].update(Vsync=False,Width=1920,Height=1080)
data["Cauldron"]["FPSLimiter"].update(Enable=False,UseGPULimiter=False)
config.write_text(json.dumps(data,indent=2))
print(f"Prepared deterministic benchmark host at {sdk}; build FFX_BRIXELIZER_GI ReleaseDX12")
