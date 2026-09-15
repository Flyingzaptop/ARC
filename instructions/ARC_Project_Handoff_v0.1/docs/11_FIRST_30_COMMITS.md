# 11 — First 30 commits

A concrete implementation sequence.

1. Initialize repository and CMake.
2. Add formatting/linting conventions.
3. Add unit-test framework.
4. Add `arc-core` static/shared library target.
5. Implement stable ID allocator.
6. Implement monotonic clock abstraction.
7. Define event enums and binary header.
8. Implement thread-local event ring.
9. Implement background event collector.
10. Implement session metadata writer.
11. Add Windows hardware discovery.
12. Add DXGI adapter discovery.
13. Add dynamic video-memory budget reader.
14. Create `arc-calibrate`.
15. Add RAM bandwidth microbenchmark.
16. Add upload/readback microbenchmark.
17. Add storage microbenchmark.
18. Define `HardwareProfile`.
19. Create D3D12 sample app.
20. Add configurable memory-pressure workload.
21. Add resource-create observation.
22. Add resource-destroy observation.
23. Add heap observation.
24. Add descriptor/view mapping.
25. Add command-queue IDs.
26. Add present/frame IDs.
27. Add barrier/copy events.
28. Implement initial resource graph.
29. Implement trace parser and ground-truth checker.
30. Publish first overhead/correctness benchmark.

At commit 30 ARC still does not optimize anything. That is intentional.
