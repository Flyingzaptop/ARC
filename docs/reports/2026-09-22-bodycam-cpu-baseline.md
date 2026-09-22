# Bodycam clean CPU baseline — 2026-09-22

User performed a 30-second moving route in Bodycam-Win64-Shipping.exe PID 33720. Module inventory verified no ARC DLL in launcher or renderer. WPR custom ARCCPU profile captured CPU samples, context switches, ReadyThread, loader/process lifetime and hard faults with SampledProfile/CSwitch/ReadyThread stacks. Elevated WPR recording was explicitly confirmed by the user. ETL spans about 34.05 s including setup/finalization; script requested 30 s gameplay capture and reports 30.41 s. WPR trace has 0 lost events and 0 lost buffers. ETL contains system-wide scheduling data and stays local; it is not included in Git.

A separate read-only arc-process-sample snapshot covered 20.005 s inside the route. It measured 130,265.625 ms of process CPU time: 6.512 logical CPU cores on average (not 6.512 fully busy game threads at every instant). 144 initial threads sampled; newly created threads were not sampled individually. Relative time by thread-name grouping:

| Group | CPU time / 20 s | Share of process CPU |
|---|---:|---:|
| Task workers | 72.34 s | 55.5% |
| Render/RHI | 21.69 s | 16.6% |
| Bink | 18.64 s | 14.3% |
| Other | 17.56 s | 13.5% |

The highest individually observed thread was RenderThread 0: 48.9% of one logical CPU. Foreground Worker #1 and #0 were 39.1% and 38.7%; RHIThread 34.1%; RHISubmissionThread 25.4%. These are averages over the 20-second subwindow, not per-frame critical-path measurements.

In process-filtered ETW Sampled Profile hits, exclusive module shares were Bodycam-Win64-Shipping.exe 68.1%, nvwgf2um.dll 10.9%, ntkrnlmp.exe 11.0%, D3D12Core.dll 1.8%. Sample hits are not equivalent to elapsed CPU milliseconds or a proof that the engine module samples are object traversal. The game ships no local PDB in its Win64 directory; the profile groups the game binary as unknown functions. Stack capture exists, but object/LOD call sites are not named.

This proves substantial parallel CPU load (not merely one underused core). It does not prove the CPU critical path versus GPU for the same 30-second frames; no simultaneous PresentMon or GPU timestamp capture was taken. Earlier 60-second clean PresentMon window measured 47.56 FPS and 18.25 ms GPU Busy, but it is a different window and cannot be merged as per-frame attribution. No CPU optimization was applied or claimed by this baseline.

Next experiment for PRIMARY: extract stable hot module offsets/stacks in RenderThread and task workers and correlate them with frame starts on a matched route. Only then choose a transformation whose inputs, side effects and result consumers can be proved. Blind LOD/object removal after DX12 draw recording cannot undo game CPU traversal. A source/engine symbol map would narrow attribution but is not assumed available.

Evidence (local only):
C:/Users/r3d_flzp/ARC-Hardening-GPU/universal-optimizer/bodycam-check-20260922/cpu-baseline-20260922-205850/cpu.etl, threads.json, profile.txt, threads-etw.txt, stacks-cpu-samples.txt, trace-stats.txt, analysis.json and manifest.json.
