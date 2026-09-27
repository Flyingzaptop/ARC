# Isolated original CPU loop replay

This experiment measures the captured loop by executing its copied original
machine code in a separate Windows process. It never injects into or modifies
the source process and never authorizes live replacement.

The manifest points to the captured module PE image, entry and natural-exit
contexts, and before/after VM-region images. preflight.py checks the
observed two-iteration instruction and memory coverage, the contiguous
original loop and append spans, and the natural iteration count from the
entry/exit induction register. prepare.py checks that the copied append
header and entire worst-case output span fit without growth, then writes a
small replay pack. These commands do not execute the native child:

    python experiments/cpu-composite-native/preflight.py PATH_TO_NATIVE_MANIFEST
    python experiments/cpu-composite-native/launch.py PATH_TO_NATIVE_MANIFEST
    powershell -NoProfile -ExecutionPolicy Bypass -File experiments/cpu-composite-native/build.ps1

The native child maps the PE and all captured regions at their exact original
addresses, checks sampled instruction bytes, and restores the captured GPR,
XMM, MXCSR, flags, and stack values with a MASM thunk. Both observed
GS:[0x58] loads are replaced in the private code image with same-span
RIP-relative loads from one nearby literal containing the captured TLS
pointer. Every out-of-span direct call and the append growth target becomes
a breakpoint trap. Natural fallthrough at the original loop end jumps to a
host restore thunk. The child compares the appended record bytes, cursor
header, and exit register/vector context with captured after-state.

The compiled child is not launched by build or prepare. After reviewing the
manifest, patches, and static gate, an explicit --execute on launch.py
starts it with a 20-second default timeout and no error dialog. Any address
collision, unsupported path, trap, timeout, or mismatch fails the measurement.
The reported time includes the entry and exit thunks; it is a bounded original
CPU instruction-span cost for this captured input and process image.

Restored repeats copy every captured before-view outside the timed interval.
After the first successful replay, the child audits changes in its own mapped
clone against the captured before-view. A warm repeat is admitted only when
changes are confined to the stack, cursor header, and fully overwritten
record range (plus intentional private code patches). It resets the stack
and cursor outside each warm timed interval while preserving other views.
Each warm run must again match output bytes and exit context, and its own
effect audit must show no writes outside the allowed ranges. Use
--warm-repeats N on launch.py only after a fully verified first replay.
warm_preflight.py reports differences in the original live capture for
diagnostics; those differences do not determine isolated warm eligibility.

The word-scatter loop uses a separate v3 pack built by prepare_scatter.py or
launch.py --scatter. Its output pointer, four-byte store, register-bound tail,
and stack-write addresses come from the captured machine-code contract. All
other calls in the loop are trapped. The output view is allocated with its
captured PAGE_READWRITE | PAGE_WRITECOMBINE protection (0x404), and the child
checks that protection with VirtualQuery before timing. Its golden comparison
uses the derived output word range, declared stack ranges, and natural exit
context. The append v2 pack remains supported.

The captured after-images were collected from a live process without a
coherent snapshot proof. Only append records, cursor metadata, and exit
context are used as golden comparisons. General source ownership, future
inputs, scheduling, and publication timing remain unresolved.
