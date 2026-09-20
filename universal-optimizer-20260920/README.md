# Universal optimizer checkpoint — not full-plan acceptance

Source branch: `codex_den/generic-dx12-runtime`, checkpoint `8a4baf9`.
Archives contain measured frames, images, quality results and native test reports.
`index.json` records archive hashes. Captured vendor shader bytecode/IR and caches
are deliberately excluded.

The three alternating heavy-scene pairs show 6.76–8.07% more FPS with the fixed
image limits passing at the saved pose. Earlier additional poses are included.
This is not a 2x FPS result. CPU overhead is still above the requested budget.

Wicked demonstrates actual generic-DLL neutral/coarse execution with host quality
actions off, but its image-quality/performance acceptance is unfinished. Live
automatic trials remain unable to retain actions without sufficient quality and
cost evidence. Geometry and temporal GI/shadow reuse are also unfinished.

See EXECUTION.md for mechanisms, checks, limitations and evidence interpretation.
