# 02 — Optimization model

## 1. Resources as competing investments

Every optional action consumes one or more budgets:

```text
VRAM
RAM
CPU time
GPU graphics time
GPU compute time
copy bandwidth
storage bandwidth
latency
risk
```

Every action produces benefits:

```text
image quality
motion quality
stutter reduction
loading reduction
latency reduction
future-risk reduction
```

Example:

```text
Action A:
keep 4K wall mip
VRAM cost: +96 MB
visual gain: low

Action B:
keep character face 4K
VRAM cost: +48 MB
visual gain: high

Action C:
enable frame generation
VRAM cost: +180 MB
compute cost: +1.2 ms
motion gain: very high
latency cost: moderate
```

The governor should not optimize any budget independently.

## 2. Action representation

Suggested internal record:

```cpp
struct CandidateAction {
    ActionId id;
    ResourceId resource;

    ActionType type;

    double visual_gain;
    double motion_gain;
    double stutter_gain;
    double latency_cost;
    double artifact_risk;

    int64_t delta_vram;
    int64_t delta_ram;
    double delta_gpu_ms;
    double delta_cpu_ms;
    double delta_copy_mb_s;

    double confidence;
    double reversibility;
};
```

## 3. Safety-first selection

Selection is two-stage.

### Stage A — feasibility

Reject actions that violate:

- semantic safety;
- memory minimums;
- synchronization rules;
- required resource dimensions;
- API validity;
- unacceptable latency;
- anti-cheat/compatibility policy.

### Stage B — utility

Rank the remaining actions by expected utility.

## 4. Short horizon vs long horizon

ARC needs at least two horizons.

### Immediate / frame horizon

Question:

> What must happen now to avoid missing frame-time or VRAM budget?

Actions:

- prevent new top mip residency;
- evict cold assets;
- reduce optional workload;
- avoid new temporal feature activation.

### Predictive horizon

Question:

> What will become useful 100–1000 ms from now?

Actions:

- prefetch;
- upgrade mip before it becomes visible;
- decode in RAM;
- reserve transient memory;
- precompile needed pipeline state where integration allows.

## 5. Hysteresis

Without hysteresis ARC will oscillate.

Bad:

```text
2K → 4K → 2K → 4K
```

Use:

- minimum residency duration;
- minimum quality hold time;
- promotion threshold higher than demotion threshold;
- cooldowns after costly transfers;
- confidence thresholds.

## 6. Emergency mode

If OS budget suddenly shrinks or a game performs a large unexpected allocation:

```text
NORMAL
  ↓
PRESSURE
  ↓
EMERGENCY
```

Emergency policy prioritizes survival:

1. stop promotions;
2. cancel speculative prefetch;
3. evict cold reversible resources;
4. remove highest optional texture mips;
5. disable optional temporal features if they consume required memory;
6. preserve synchronization/data resources;
7. restore quality only after sustained stability.

## 7. Learning strategy

Do not start with RL.

Start with:

- deterministic rules;
- measured costs;
- statistical reuse prediction;
- online exponential averages.

Later add ML for:

- resource reuse probability;
- scene-change prediction;
- future VRAM demand;
- future frame-time;
- visual-impact estimation.

ML proposes or scores. Safety policy retains authority.
