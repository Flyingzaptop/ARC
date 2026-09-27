# Homogeneous one-word scatter GPU trial

`scripts/cpu_word_gpu.py` lowers the closed typed IR for a bounded homogeneous
group. It emits one output word per valid item, with branch and one-bit-mask
guards. Each row writes a GPU scratch validity word, result word, four-byte
count delta, and binary flag event. Captured process addresses and carried
output indices do not enter the shader.

Sampling and machine-code screening proposed this region without supplied
engine names or addresses. The region was then selected for a bounded local
study, and its captures and native/GPU trials were orchestrated explicitly.
The typed model and HLSL were generated from captured provenance; this is a
supported observed class, not an automatic transfer of arbitrary game code.

The separate `compaction/` shaders assign output slots with a GPU exclusive
prefix, scatter words, and aggregate nine metadata words: emitted count, final
index, final counter, binary flag OR, invalid count, and three required
last-item stack values supplied once from the bounded before-view proof. A
failed guard makes the packet invalid. Publication requires invalid count zero
and emitted count equal to the proved group count.

`bulk-bound/` binds 65,343 captured rows (34 input words each) and independent
after-view expected output words. The fused `word_bench.cpp` uploads the packed
inputs and runs generated evaluation, prefix, group aggregation, and scatter in
one command list. It reads back only the packed words and final metadata.
Compiler and device setup occur outside timing; validation follows the timed
CPU consume copy. The bounded native CPU gather is measured separately and
must be added to fused time without counting GPU timestamps again.

This is an isolated snapshot experiment. Runtime group membership, memory
ownership, first consumer timing, and full-frame benefit are not established.
The measured composed route is slower than the warm original CPU loop, so
live replacement is disabled.

Three saved native gather reruns had median 6.8016 ms; the fused GPU tail had
20-run median 1.5082 ms. The nonoverlapped composed median is 8.3098 ms,
against warm original CPU median 2.4539 ms. The original warm mean was 2.7105
ms, including a 3.987 ms outlier; the composed mean was 8.30128 ms. The
earlier single gather measurement of 6.6403 ms is retained separately.
Recompute these values from raw evidence with
`python scripts/analyze_cpu_composite_cost.py word experiments/cpu-word-gpu/results --check`.
