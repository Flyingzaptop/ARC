#ifdef _WIN32

// Reuse the already hardware-validated Stage 7 D3D12 graphics harness in this
// translation unit.  Only its command-line main is renamed; Stage 8 exercises
// the same real geometry/raster/texture/lighting/shadow passes.
#define main arc_stage7_embedded_main
#include "mixed_graphics_benchmark.cpp"
#undef main

#include "arc/adaptive_quality_controller.hpp"

#include <array>
#include <optional>
#include <set>
#include <unordered_set>

namespace stage8 {

struct Args {
    int seconds{60};
    int probe_frames{12};
    int control_frames{24};
    std::filesystem::path output{"traces/closed-loop-mixed-graphics.json"};
};

Args parse_stage8_args(int argc, char** argv) {
    Args args{};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) args.seconds = std::clamp(std::atoi(argv[++i]), 12, 600);
        else if (a == "--probe-frames" && i + 1 < argc) args.probe_frames = std::clamp(std::atoi(argv[++i]), 4, 120);
        else if (a == "--control-frames" && i + 1 < argc) args.control_frames = std::clamp(std::atoi(argv[++i]), 4, 240);
        else if (a == "--output" && i + 1 < argc) args.output = argv[++i];
    }
    return args;
}

struct SceneLoad {
    UINT geometry_add{};
    UINT raster_add{};
    UINT texture_add{};
    UINT lighting_add{};
    UINT shadow_pass_add{};
    UINT shadow_sample_add{};
};

Knobs full_quality() {
    Knobs k{};
    k.geometry_instances = 240000;
    k.raster_layers = 4;
    k.texture_samples = 32;
    k.light_iterations = 48;
    k.shadow_passes = 4;
    k.shadow_samples = 12;
    return k;
}

UINT add_sat(UINT a, UINT b) {
    if (b > UINT_MAX - a) return UINT_MAX;
    return a + b;
}

Knobs effective_knobs(const Knobs& quality, const SceneLoad& scene) {
    Knobs out = quality;
    out.geometry_instances = add_sat(out.geometry_instances, scene.geometry_add);
    out.raster_layers = add_sat(out.raster_layers, scene.raster_add);
    out.texture_samples = add_sat(out.texture_samples, scene.texture_add);
    out.light_iterations = add_sat(out.light_iterations, scene.lighting_add);
    out.shadow_passes = add_sat(out.shadow_passes, scene.shadow_pass_add);
    out.shadow_samples = add_sat(out.shadow_samples, scene.shadow_sample_add);
    return out;
}

struct LadderDefinition {
    std::uint64_t id{};
    arc::QualityDomain domain{};
    const char* label{};
    std::array<double, 3> visual_cost{};
    double confidence{};
};

constexpr std::array<LadderDefinition, 5> kLadders{{
    {810, arc::QualityDomain::Bandwidth, "far texture sampling", {0.025, 0.040, 0.065}, 0.96},
    {820, arc::QualityDomain::Raster,    "distant raster overdraw", {0.045, 0.070, 0.105}, 0.97},
    {830, arc::QualityDomain::Geometry,  "distant geometry density", {0.025, 0.045, 0.075}, 0.95},
    {840, arc::QualityDomain::Lighting,  "far lighting iterations", {0.035, 0.060, 0.095}, 0.96},
    {850, arc::QualityDomain::Shadow,    "far shadow quality", {0.030, 0.055, 0.090}, 0.95},
}};

const LadderDefinition* ladder_for(std::uint64_t id) {
    for (const auto& d : kLadders) if (d.id == id) return &d;
    return nullptr;
}

void set_quality_level(std::uint64_t id, std::uint32_t level, Knobs& k) {
    // level 0 = full quality, 1..3 = progressively cheaper states.
    level = std::min<std::uint32_t>(3, level);
    switch (id) {
    case 810: {
        static constexpr UINT v[]{32, 24, 16, 8};
        k.texture_samples = v[level];
        break;
    }
    case 820: {
        static constexpr UINT v[]{4, 3, 2, 1};
        k.raster_layers = v[level];
        break;
    }
    case 830: {
        static constexpr UINT v[]{240000, 180000, 120000, 60000};
        k.geometry_instances = v[level];
        break;
    }
    case 840: {
        static constexpr UINT v[]{48, 36, 24, 12};
        k.light_iterations = v[level];
        break;
    }
    case 850: {
        static constexpr UINT passes[]{4, 3, 2, 1};
        static constexpr UINT samples[]{12, 9, 6, 4};
        k.shadow_passes = passes[level];
        k.shadow_samples = samples[level];
        break;
    }
    default: break;
    }
}

void apply_degrade(const arc::QualityActionCandidate& action, Knobs& k) {
    set_quality_level(action.id, action.sequence + 1, k);
}

void apply_restore(const arc::QualityActionCandidate& action, Knobs& k) {
    set_quality_level(action.id, action.sequence, k);
}

bool same_quality(const Knobs& a, const Knobs& b) {
    return a.geometry_instances == b.geometry_instances &&
           a.raster_layers == b.raster_layers &&
           a.texture_samples == b.texture_samples &&
           a.light_iterations == b.light_iterations &&
           a.shadow_passes == b.shadow_passes &&
           a.shadow_samples == b.shadow_samples;
}

struct ProbeResult {
    std::uint64_t id{};
    std::uint32_t sequence{};
    arc::QualityDomain domain{};
    std::string label{};
    double visual_cost{};
    double confidence{};
    double before_p50_ms{};
    double after_p50_ms{};
    double measured_gain_ms{};
};

std::vector<ProbeResult> calibrate_quality_ladders(
    GraphicsHarness& harness,
    const Knobs& full,
    int probe_frames,
    UINT& salt) {
    std::vector<ProbeResult> probes;
    probes.reserve(kLadders.size() * 3);

    for (const auto& ladder : kLadders) {
        Knobs current = full;
        double before = harness.probe(current, probe_frames, salt);
        salt += 1000;
        for (std::uint32_t sequence = 0; sequence < 3; ++sequence) {
            Knobs next = current;
            set_quality_level(ladder.id, sequence + 1, next);
            const double after = harness.probe(next, probe_frames, salt);
            salt += 1000;
            ProbeResult p{};
            p.id = ladder.id;
            p.sequence = sequence;
            p.domain = ladder.domain;
            p.label = std::string(ladder.label) + " step " + std::to_string(sequence);
            p.visual_cost = ladder.visual_cost[sequence];
            p.confidence = ladder.confidence;
            p.before_p50_ms = before;
            p.after_p50_ms = after;
            p.measured_gain_ms = std::max(0.0, before - after);
            probes.push_back(std::move(p));
            current = next;
            before = after;
        }
    }
    return probes;
}

SceneLoad load_for(arc::QualityDomain domain, int intensity) {
    SceneLoad load{};
    const UINT n = static_cast<UINT>(std::max(1, intensity));
    switch (domain) {
    case arc::QualityDomain::Bandwidth:
    case arc::QualityDomain::Texture:
        load.texture_add = 16u * n;
        break;
    case arc::QualityDomain::Raster:
        load.raster_add = n;
        break;
    case arc::QualityDomain::Geometry:
        load.geometry_add = 180000u * n;
        break;
    case arc::QualityDomain::Lighting:
        load.lighting_add = 16u * n;
        break;
    case arc::QualityDomain::Shadow:
        load.shadow_pass_add = 2u * n;
        load.shadow_sample_add = 8u * n;
        break;
    case arc::QualityDomain::Temporal:
        break;
    }
    return load;
}

struct SceneCalibration {
    arc::QualityDomain domain{};
    int intensity{};
    SceneLoad load{};
    double full_quality_p50_ms{};
};

SceneCalibration calibrate_heavy_scene(
    GraphicsHarness& harness,
    const Knobs& full,
    arc::QualityDomain domain,
    double easy_p50,
    int probe_frames,
    UINT& salt) {
    SceneCalibration out{};
    out.domain = domain;
    const double desired = easy_p50 * 1.25;
    for (int intensity = 1; intensity <= 8; ++intensity) {
        out.intensity = intensity;
        out.load = load_for(domain, intensity);
        out.full_quality_p50_ms = harness.probe(
            effective_knobs(full, out.load), probe_frames, salt);
        salt += 1000;
        if (out.full_quality_p50_ms >= desired) break;
    }
    return out;
}

struct PhaseSpec {
    std::string name{};
    std::optional<arc::QualityDomain> heavy_domain{};
    SceneLoad load{};
    int weight{1};
};

struct PhaseMetrics {
    std::string name{};
    int domain{-1};
    std::vector<double> gpu_ms{};
    std::uint64_t over_budget_frames{};
    double overshoot_sum_ms{};
    std::uint32_t action_events{};
    std::uint32_t direction_flips{};
};

struct RunMetrics {
    std::vector<double> gpu_ms{};
    std::vector<PhaseMetrics> phases{};
    std::uint64_t over_budget_frames{};
    double overshoot_sum_ms{};
    Budget budget_peak{};
};

void record_frame(RunMetrics& run, PhaseMetrics& phase, double ms, double target_ms) {
    if (!(ms > 0.0) || !std::isfinite(ms)) return;
    run.gpu_ms.push_back(ms);
    phase.gpu_ms.push_back(ms);
    if (ms > target_ms) {
        ++run.over_budget_frames;
        ++phase.over_budget_frames;
        const double over = ms - target_ms;
        run.overshoot_sum_ms += over;
        phase.overshoot_sum_ms += over;
    }
}

int total_weight(const std::vector<PhaseSpec>& phases) {
    int total = 0;
    for (const auto& phase : phases) total += std::max(1, phase.weight);
    return std::max(1, total);
}

std::chrono::milliseconds phase_duration(int pass_seconds, int weight, int weights) {
    const auto total_ms = static_cast<long long>(pass_seconds) * 1000ll;
    return std::chrono::milliseconds(std::max<long long>(250, total_ms * std::max(1, weight) / weights));
}

RunMetrics run_baseline_schedule(
    GraphicsHarness& harness,
    GpuContext& ctx,
    const Knobs& full,
    const std::vector<PhaseSpec>& phases,
    int pass_seconds,
    double target_ms,
    UINT& salt) {
    RunMetrics run{};
    run.budget_peak = query_budget(ctx);
    const int weights = total_weight(phases);

    for (const auto& spec : phases) {
        PhaseMetrics phase{};
        phase.name = spec.name;
        phase.domain = spec.heavy_domain ? static_cast<int>(*spec.heavy_domain) : -1;
        const auto deadline = std::chrono::steady_clock::now() + phase_duration(pass_seconds, spec.weight, weights);
        const Knobs effective = effective_knobs(full, spec.load);
        while (std::chrono::steady_clock::now() < deadline) {
            const double ms = harness.render_frame(effective, salt++);
            record_frame(run, phase, ms, target_ms);
            const auto b = query_budget(ctx);
            run.budget_peak.usage = std::max(run.budget_peak.usage, b.usage);
            run.budget_peak.budget = b.budget;
        }
        run.phases.push_back(std::move(phase));
    }
    return run;
}

arc::FrameBudgetSample control_sample(
    double frame_ms,
    double target_ms,
    const std::optional<arc::QualityDomain>& domain,
    const Budget& budget) {
    arc::FrameBudgetSample s{};
    s.frame_ms = frame_ms;
    s.target_frame_ms = target_ms;
    s.gpu_busy_fraction = 0.99;
    s.memory_bandwidth_fraction = 0.20;
    s.raster_pressure = 0.20;
    s.geometry_pressure = 0.20;
    s.lighting_pressure = 0.20;
    s.shadow_pressure = 0.20;
    s.local_usage_bytes = budget.usage;
    s.local_budget_bytes = budget.budget;

    if (domain) {
        switch (*domain) {
        case arc::QualityDomain::Bandwidth:
        case arc::QualityDomain::Texture: s.memory_bandwidth_fraction = 0.98; break;
        case arc::QualityDomain::Raster: s.raster_pressure = 0.98; break;
        case arc::QualityDomain::Geometry: s.geometry_pressure = 0.98; break;
        case arc::QualityDomain::Lighting: s.lighting_pressure = 0.98; break;
        case arc::QualityDomain::Shadow: s.shadow_pressure = 0.98; break;
        case arc::QualityDomain::Temporal: break;
        }
    }
    return s;
}

struct ActionEvent {
    std::size_t phase_index{};
    std::string phase{};
    arc::QualityDecisionKind kind{arc::QualityDecisionKind::None};
    std::uint64_t id{};
    std::uint32_t sequence{};
    arc::QualityDomain domain{};
    std::string label{};
    double before_ms{};
    double after_ms{};
    double expected_gain_ms{};
    double observed_delta_ms{};
    std::size_t active_after{};
    Knobs quality_after{};
};

struct AdaptiveRun {
    RunMetrics metrics{};
    std::vector<ActionEvent> events{};
    Knobs final_quality{};
    arc::AdaptiveQualityControllerState controller_state{};
    bool temporal_used{};
    std::set<int> degraded_domains{};
};

AdaptiveRun run_adaptive_schedule(
    GraphicsHarness& harness,
    GpuContext& ctx,
    const Knobs& full,
    const std::vector<PhaseSpec>& phases,
    const std::vector<arc::QualityActionCandidate>& candidates,
    int pass_seconds,
    int control_frames,
    int effect_probe_frames,
    double target_ms,
    double easy_p50,
    UINT& salt) {
    AdaptiveRun out{};
    out.metrics.budget_peak = query_budget(ctx);
    Knobs quality = full;

    arc::AdaptiveQualityControllerConfig cfg{};
    cfg.optimizer.allow_temporal_assist = false;
    cfg.optimizer.minimum_gain_ms = std::max(0.0005, easy_p50 * 0.0005);
    cfg.optimizer.minimum_confidence = 0.45;
    cfg.optimizer.restoration_headroom_ms = std::max(0.002, easy_p50 * 0.030);
    cfg.optimizer.max_actions_per_plan = 8;
    cfg.overload_samples_required = 2;
    cfg.headroom_samples_required = 3;
    cfg.settle_samples_after_change = 1;
    cfg.minimum_hold_samples_after_degrade = 4;
    cfg.frame_ewma_alpha = 0.55;
    cfg.overload_margin_ms = std::max(0.001, easy_p50 * 0.008);
    cfg.extra_restore_headroom_ms = std::max(0.001, easy_p50 * 0.012);
    cfg.max_actions_per_decision = 1;
    cfg.max_active_actions = 15;
    arc::AdaptiveQualityController controller{cfg};

    const int weights = total_weight(phases);
    std::vector<double> control_window;
    control_window.reserve(static_cast<std::size_t>(control_frames));

    for (std::size_t phase_index = 0; phase_index < phases.size(); ++phase_index) {
        const auto& spec = phases[phase_index];
        PhaseMetrics phase{};
        phase.name = spec.name;
        phase.domain = spec.heavy_domain ? static_cast<int>(*spec.heavy_domain) : -1;
        const auto deadline = std::chrono::steady_clock::now() + phase_duration(pass_seconds, spec.weight, weights);
        arc::QualityDecisionKind last_event_kind = arc::QualityDecisionKind::None;
        control_window.clear();

        while (std::chrono::steady_clock::now() < deadline) {
            const Knobs effective = effective_knobs(quality, spec.load);
            const double ms = harness.render_frame(effective, salt++);
            record_frame(out.metrics, phase, ms, target_ms);
            if (ms > 0.0 && std::isfinite(ms)) control_window.push_back(ms);

            const auto b = query_budget(ctx);
            out.metrics.budget_peak.usage = std::max(out.metrics.budget_peak.usage, b.usage);
            out.metrics.budget_peak.budget = b.budget;

            if (static_cast<int>(control_window.size()) < control_frames) continue;
            const double before = percentile(control_window, 0.50);
            control_window.clear();

            const auto decision = controller.tick(control_sample(before, target_ms, spec.heavy_domain, b), candidates);
            if (decision.kind == arc::QualityDecisionKind::None || decision.plan.actions.empty()) continue;

            const auto action = decision.plan.actions.front();
            out.temporal_used = out.temporal_used || action.temporal_assist || action.domain == arc::QualityDomain::Temporal;
            if (out.temporal_used) continue;

            if (decision.kind == arc::QualityDecisionKind::Degrade) {
                apply_degrade(action, quality);
                out.degraded_domains.insert(static_cast<int>(action.domain));
            } else {
                apply_restore(action, quality);
            }

            const double after = harness.probe(
                effective_knobs(quality, spec.load), effect_probe_frames, salt);
            salt += static_cast<UINT>(effect_probe_frames + 1);
            controller.note_action_applied(action, decision.kind, before, after, true);

            ActionEvent event{};
            event.phase_index = phase_index;
            event.phase = spec.name;
            event.kind = decision.kind;
            event.id = action.id;
            event.sequence = action.sequence;
            event.domain = action.domain;
            event.label = action.label;
            event.before_ms = before;
            event.after_ms = after;
            event.expected_gain_ms = action.expected_ms_gain;
            event.observed_delta_ms = decision.kind == arc::QualityDecisionKind::Degrade
                ? before - after : after - before;
            event.active_after = controller.active_actions().size();
            event.quality_after = quality;
            out.events.push_back(std::move(event));
            ++phase.action_events;

            if (last_event_kind != arc::QualityDecisionKind::None && last_event_kind != decision.kind) {
                ++phase.direction_flips;
            }
            last_event_kind = decision.kind;
        }
        out.metrics.phases.push_back(std::move(phase));
    }

    out.final_quality = quality;
    out.controller_state = controller.state();
    return out;
}

struct AggregateSummary {
    std::size_t samples{};
    double p50_ms{};
    double p95_ms{};
    double p99_ms{};
    double miss_ratio{};
    double mean_overshoot_ms{};
};

AggregateSummary summarize(const RunMetrics& run) {
    AggregateSummary s{};
    s.samples = run.gpu_ms.size();
    s.p50_ms = percentile(run.gpu_ms, 0.50);
    s.p95_ms = percentile(run.gpu_ms, 0.95);
    s.p99_ms = percentile(run.gpu_ms, 0.99);
    if (!run.gpu_ms.empty()) {
        s.miss_ratio = static_cast<double>(run.over_budget_frames) / static_cast<double>(run.gpu_ms.size());
        s.mean_overshoot_ms = run.overshoot_sum_ms / static_cast<double>(run.gpu_ms.size());
    }
    return s;
}

const char* kind_name(arc::QualityDecisionKind kind) {
    switch (kind) {
    case arc::QualityDecisionKind::Degrade: return "degrade";
    case arc::QualityDecisionKind::Restore: return "restore";
    case arc::QualityDecisionKind::None: return "none";
    }
    return "none";
}

void write_phase_array(std::ostream& f, const std::vector<PhaseMetrics>& phases) {
    f << "[\n";
    for (std::size_t i = 0; i < phases.size(); ++i) {
        const auto& p = phases[i];
        const double miss = p.gpu_ms.empty() ? 0.0 :
            static_cast<double>(p.over_budget_frames) / static_cast<double>(p.gpu_ms.size());
        const double over = p.gpu_ms.empty() ? 0.0 : p.overshoot_sum_ms / static_cast<double>(p.gpu_ms.size());
        f << "    {\"name\":\"" << json_escape(p.name) << "\",\"domain\":" << p.domain
          << ",\"samples\":" << p.gpu_ms.size()
          << ",\"p50_ms\":" << percentile(p.gpu_ms, 0.50)
          << ",\"p95_ms\":" << percentile(p.gpu_ms, 0.95)
          << ",\"p99_ms\":" << percentile(p.gpu_ms, 0.99)
          << ",\"miss_ratio\":" << miss
          << ",\"mean_overshoot_ms\":" << over
          << ",\"action_events\":" << p.action_events
          << ",\"direction_flips\":" << p.direction_flips << "}";
        if (i + 1 != phases.size()) f << ',';
        f << '\n';
    }
    f << "  ]";
}

void write_report(
    const std::filesystem::path& path,
    const GpuContext& ctx,
    const Knobs& full,
    double easy_p50,
    double target_ms,
    int control_frames,
    const std::vector<ProbeResult>& probes,
    const std::vector<SceneCalibration>& scenes,
    const RunMetrics& baseline,
    const AdaptiveRun& adaptive) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path);
    if (!f) {
        std::cerr << "Could not open report: " << path.string() << "\n";
        std::exit(4);
    }

    const auto b = summarize(baseline);
    const auto a = summarize(adaptive.metrics);
    const double miss_reduction = b.miss_ratio > 0.0 ? (b.miss_ratio - a.miss_ratio) / b.miss_ratio : 0.0;
    const double overshoot_reduction = b.mean_overshoot_ms > 0.0
        ? (b.mean_overshoot_ms - a.mean_overshoot_ms) / b.mean_overshoot_ms : 0.0;

    std::size_t degrade_events = 0;
    std::size_t restore_events = 0;
    for (const auto& e : adaptive.events) {
        if (e.kind == arc::QualityDecisionKind::Degrade) ++degrade_events;
        else if (e.kind == arc::QualityDecisionKind::Restore) ++restore_events;
    }
    std::uint32_t max_phase_flips = 0;
    for (const auto& p : adaptive.metrics.phases) max_phase_flips = std::max(max_phase_flips, p.direction_flips);

    f << std::fixed << std::setprecision(6);
    f << "{\n";
    f << "  \"schema\": 1,\n";
    f << "  \"valid\": true,\n";
    f << "  \"benchmark\": \"closed_loop_mixed_graphics\",\n";
    f << "  \"adapter\": \"" << json_escape(ctx.adapter_name) << "\",\n";
    f << "  \"native_width\": " << GraphicsHarness::kWidth << ",\n";
    f << "  \"native_height\": " << GraphicsHarness::kHeight << ",\n";
    f << "  \"temporal_enabled\": false,\n";
    f << "  \"temporal_used\": " << (adaptive.temporal_used ? "true" : "false") << ",\n";
    f << "  \"easy_full_quality_p50_ms\": " << easy_p50 << ",\n";
    f << "  \"target_frame_ms\": " << target_ms << ",\n";
    f << "  \"control_window_frames\": " << control_frames << ",\n";
    f << "  \"full_quality\": "; write_knobs(f, full); f << ",\n";
    f << "  \"final_quality\": "; write_knobs(f, adaptive.final_quality); f << ",\n";

    f << "  \"baseline\": {\"samples\":" << b.samples << ",\"p50_ms\":" << b.p50_ms
      << ",\"p95_ms\":" << b.p95_ms << ",\"p99_ms\":" << b.p99_ms
      << ",\"miss_ratio\":" << b.miss_ratio << ",\"mean_overshoot_ms\":" << b.mean_overshoot_ms
      << ",\"dxgi_peak_usage\":" << baseline.budget_peak.usage
      << ",\"dxgi_budget\":" << baseline.budget_peak.budget << "},\n";
    f << "  \"adaptive\": {\"samples\":" << a.samples << ",\"p50_ms\":" << a.p50_ms
      << ",\"p95_ms\":" << a.p95_ms << ",\"p99_ms\":" << a.p99_ms
      << ",\"miss_ratio\":" << a.miss_ratio << ",\"mean_overshoot_ms\":" << a.mean_overshoot_ms
      << ",\"dxgi_peak_usage\":" << adaptive.metrics.budget_peak.usage
      << ",\"dxgi_budget\":" << adaptive.metrics.budget_peak.budget << "},\n";
    f << "  \"delta\": {\"miss_ratio\":" << (a.miss_ratio - b.miss_ratio)
      << ",\"miss_reduction_fraction\":" << miss_reduction
      << ",\"mean_overshoot_ms\":" << (a.mean_overshoot_ms - b.mean_overshoot_ms)
      << ",\"overshoot_reduction_fraction\":" << overshoot_reduction << "},\n";

    f << "  \"controller\": {\"degrade_events\":" << degrade_events
      << ",\"restore_events\":" << restore_events
      << ",\"direction_changes\":" << adaptive.controller_state.direction_changes
      << ",\"max_phase_direction_flips\":" << max_phase_flips
      << ",\"unique_degraded_domains\":" << adaptive.degraded_domains.size()
      << ",\"final_active_actions\":" << adaptive.controller_state.active_actions << "},\n";

    f << "  \"quality_probes\": [\n";
    for (std::size_t i = 0; i < probes.size(); ++i) {
        const auto& p = probes[i];
        f << "    {\"id\":" << p.id << ",\"sequence\":" << p.sequence
          << ",\"domain\":" << static_cast<int>(p.domain)
          << ",\"label\":\"" << json_escape(p.label) << "\",\"before_p50_ms\":" << p.before_p50_ms
          << ",\"after_p50_ms\":" << p.after_p50_ms
          << ",\"measured_gain_ms\":" << p.measured_gain_ms
          << ",\"visual_cost\":" << p.visual_cost << "}";
        if (i + 1 != probes.size()) f << ',';
        f << '\n';
    }
    f << "  ],\n";

    f << "  \"scene_calibration\": [\n";
    for (std::size_t i = 0; i < scenes.size(); ++i) {
        const auto& s = scenes[i];
        f << "    {\"domain\":" << static_cast<int>(s.domain)
          << ",\"intensity\":" << s.intensity
          << ",\"full_quality_p50_ms\":" << s.full_quality_p50_ms << "}";
        if (i + 1 != scenes.size()) f << ',';
        f << '\n';
    }
    f << "  ],\n";

    f << "  \"events\": [\n";
    for (std::size_t i = 0; i < adaptive.events.size(); ++i) {
        const auto& e = adaptive.events[i];
        f << "    {\"phase_index\":" << e.phase_index << ",\"phase\":\"" << json_escape(e.phase)
          << "\",\"kind\":\"" << kind_name(e.kind) << "\",\"id\":" << e.id
          << ",\"sequence\":" << e.sequence << ",\"domain\":" << static_cast<int>(e.domain)
          << ",\"label\":\"" << json_escape(e.label) << "\",\"before_ms\":" << e.before_ms
          << ",\"after_ms\":" << e.after_ms << ",\"expected_gain_ms\":" << e.expected_gain_ms
          << ",\"observed_delta_ms\":" << e.observed_delta_ms
          << ",\"active_after\":" << e.active_after << ",\"quality_after\":";
        write_knobs(f, e.quality_after);
        f << "}";
        if (i + 1 != adaptive.events.size()) f << ',';
        f << '\n';
    }
    f << "  ],\n";

    f << "  \"baseline_phases\": "; write_phase_array(f, baseline.phases); f << ",\n";
    f << "  \"adaptive_phases\": "; write_phase_array(f, adaptive.metrics.phases); f << "\n";
    f << "}\n";
}

} // namespace stage8

int main(int argc, char** argv) {
    using namespace stage8;
    const auto args = parse_stage8_args(argc, argv);

    GpuContext ctx;
    ctx.init();
    GraphicsHarness harness(ctx);
    const Knobs full = full_quality();

    for (int i = 0; i < 16; ++i) harness.render_frame(full, static_cast<UINT>(i + 1));

    UINT salt = 10000;
    const double easy_p50 = harness.probe(full, std::max(8, args.probe_frames), salt);
    salt += 2000;
    if (!(easy_p50 > 0.0) || !std::isfinite(easy_p50)) {
        std::cerr << "Could not calibrate easy full-quality GPU frame time.\n";
        return 3;
    }
    const double target_ms = easy_p50 * 1.10;

    std::cout << "ARC Stage 8 closed-loop mixed graphics benchmark on " << ctx.adapter_name << "\n";
    std::cout << "Native target: " << GraphicsHarness::kWidth << "x" << GraphicsHarness::kHeight << "\n";
    std::cout << "Temporal / DLSS / FSR / Frame Generation / dynamic resolution: OFF\n";
    std::cout << std::fixed << std::setprecision(3)
              << "Easy full-quality P50: " << easy_p50 << " ms; closed-loop target: " << target_ms << " ms\n";

    std::cout << "Calibrating three-step quality ladders across 5 domains\n";
    const auto probes = calibrate_quality_ladders(harness, full, args.probe_frames, salt);
    std::vector<arc::QualityActionCandidate> candidates;
    candidates.reserve(probes.size() + 1);
    for (const auto& p : probes) {
        candidates.push_back({p.id, p.domain, p.label, p.measured_gain_ms, p.visual_cost,
                              p.confidence, 0, true, false, p.sequence});
        std::cout << "  " << p.label << ": " << p.measured_gain_ms << " ms\n";
    }
    candidates.push_back({9999, arc::QualityDomain::Temporal, "temporal assist disabled",
                          easy_p50, 0.001, 1.0, 0, true, true, 0});

    const std::array<arc::QualityDomain, 5> domains{
        arc::QualityDomain::Bandwidth,
        arc::QualityDomain::Raster,
        arc::QualityDomain::Geometry,
        arc::QualityDomain::Lighting,
        arc::QualityDomain::Shadow,
    };
    std::vector<SceneCalibration> scenes;
    scenes.reserve(domains.size());
    std::cout << "Calibrating scene-load transitions\n";
    for (const auto domain : domains) {
        auto scene = calibrate_heavy_scene(harness, full, domain, easy_p50, args.probe_frames, salt);
        std::cout << "  domain " << static_cast<int>(domain) << " intensity " << scene.intensity
                  << ": " << scene.full_quality_p50_ms << " ms\n";
        scenes.push_back(scene);
    }

    auto scene_for = [&](arc::QualityDomain domain) -> SceneLoad {
        for (const auto& s : scenes) if (s.domain == domain) return s.load;
        return {};
    };

    const std::vector<PhaseSpec> schedule{
        {"easy-0", std::nullopt, {}, 1},
        {"bandwidth-heavy", arc::QualityDomain::Bandwidth, scene_for(arc::QualityDomain::Bandwidth), 2},
        {"easy-1", std::nullopt, {}, 1},
        {"raster-heavy", arc::QualityDomain::Raster, scene_for(arc::QualityDomain::Raster), 2},
        {"easy-2", std::nullopt, {}, 1},
        {"geometry-heavy", arc::QualityDomain::Geometry, scene_for(arc::QualityDomain::Geometry), 2},
        {"easy-3", std::nullopt, {}, 1},
        {"lighting-heavy", arc::QualityDomain::Lighting, scene_for(arc::QualityDomain::Lighting), 2},
        {"easy-4", std::nullopt, {}, 1},
        {"shadow-heavy", arc::QualityDomain::Shadow, scene_for(arc::QualityDomain::Shadow), 2},
        {"easy-final", std::nullopt, {}, 3},
    };

    const int pass_seconds = std::max(6, args.seconds / 2);
    std::cout << "Pass 1/2 fixed full-quality baseline dynamic schedule: " << pass_seconds << " s\n";
    const auto baseline = run_baseline_schedule(harness, ctx, full, schedule, pass_seconds, target_ms, salt);

    std::cout << "Pass 2/2 ARC closed-loop dynamic schedule: " << pass_seconds << " s\n";
    const auto adaptive = run_adaptive_schedule(
        harness, ctx, full, schedule, candidates, pass_seconds, args.control_frames,
        std::max(4, std::min(args.probe_frames, 12)), target_ms, easy_p50, salt);

    write_report(args.output, ctx, full, easy_p50, target_ms, args.control_frames,
                 probes, scenes, baseline, adaptive);

    const auto b = summarize(baseline);
    const auto a = summarize(adaptive.metrics);
    const double miss_reduction = b.miss_ratio > 0.0 ? (b.miss_ratio - a.miss_ratio) / b.miss_ratio : 0.0;
    const double over_reduction = b.mean_overshoot_ms > 0.0
        ? (b.mean_overshoot_ms - a.mean_overshoot_ms) / b.mean_overshoot_ms : 0.0;
    std::size_t degrade_events = 0, restore_events = 0;
    for (const auto& e : adaptive.events) {
        if (e.kind == arc::QualityDecisionKind::Degrade) ++degrade_events;
        else if (e.kind == arc::QualityDecisionKind::Restore) ++restore_events;
    }

    std::cout << std::fixed << std::setprecision(3)
              << "Baseline miss ratio: " << (100.0 * b.miss_ratio) << "%\n"
              << "ARC miss ratio:      " << (100.0 * a.miss_ratio) << "%\n"
              << "Miss reduction:      " << (100.0 * miss_reduction) << "%\n"
              << "Overshoot reduction: " << (100.0 * over_reduction) << "%\n"
              << "Actions: degrade=" << degrade_events << " restore=" << restore_events
              << " unique domains=" << adaptive.degraded_domains.size() << "\n"
              << "Final active actions: " << adaptive.controller_state.active_actions << "\n"
              << "Temporal used: " << (adaptive.temporal_used ? "YES" : "NO") << "\n"
              << "Report: " << args.output.string() << "\n";

    const bool scenes_heavy = std::all_of(scenes.begin(), scenes.end(), [&](const auto& s) {
        return s.full_quality_p50_ms > target_ms;
    });
    const bool valid = !baseline.gpu_ms.empty() && !adaptive.metrics.gpu_ms.empty() &&
        scenes_heavy && !adaptive.temporal_used && degrade_events >= 3 && restore_events >= 2;
    return valid ? 0 : 3;
}

#else
int main() { return 77; }
#endif
