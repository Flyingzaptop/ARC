#include "arc/adaptive_quality_controller.hpp"

#include <cassert>
#include <iostream>
#include <vector>

using namespace arc;

namespace {

AdaptiveQualityControllerConfig immediate_config() {
    AdaptiveQualityControllerConfig cfg{};
    cfg.overload_samples_required = 1;
    cfg.headroom_samples_required = 1;
    cfg.settle_samples_after_change = 0;
    cfg.minimum_hold_samples_after_degrade = 0;
    cfg.frame_ewma_alpha = 1.0;
    cfg.overload_margin_ms = 0.0;
    cfg.extra_restore_headroom_ms = 0.0;
    cfg.optimizer.minimum_gain_ms = 0.01;
    cfg.optimizer.minimum_confidence = 0.50;
    cfg.optimizer.restoration_headroom_ms = 0.5;
    return cfg;
}

FrameBudgetSample lighting_sample(double frame_ms, double target_ms = 16.7) {
    FrameBudgetSample s{};
    s.frame_ms = frame_ms;
    s.target_frame_ms = target_ms;
    s.gpu_busy_fraction = 0.99;
    s.lighting_pressure = 0.95;
    return s;
}

} // namespace

int main() {
    // Basic closed-loop degrade -> settle -> restore flow. Temporal remains
    // forbidden by default even when it has a much larger nominal gain.
    {
        AdaptiveQualityControllerConfig cfg{};
        cfg.overload_samples_required = 2;
        cfg.headroom_samples_required = 2;
        cfg.settle_samples_after_change = 1;
        cfg.minimum_hold_samples_after_degrade = 0;
        cfg.frame_ewma_alpha = 1.0;
        cfg.overload_margin_ms = 0.0;
        cfg.extra_restore_headroom_ms = 0.0;
        AdaptiveQualityController controller{cfg};

        auto overloaded = lighting_sample(22.0);
        std::vector<QualityActionCandidate> candidates{
            {1, QualityDomain::Lighting, "far light update", 2.0, 0.05, 0.95, 0, true, false, 0},
            {2, QualityDomain::Temporal, "DLSS", 8.0, 0.01, 1.0, 0, true, true, 0},
        };

        auto first = controller.tick(overloaded, candidates);
        assert(first.kind == QualityDecisionKind::None);
        auto second = controller.tick(overloaded, candidates);
        assert(second.kind == QualityDecisionKind::Degrade);
        assert(!second.plan.actions.empty());
        for (const auto& a : second.plan.actions) assert(a.domain != QualityDomain::Temporal);

        const auto action = second.plan.actions.front();
        controller.note_action_applied(action, QualityDecisionKind::Degrade, 22.0, 19.0, true);
        assert(controller.active_actions().size() == 1);

        auto settling = controller.tick(overloaded, candidates);
        assert(settling.kind == QualityDecisionKind::None);

        auto headroom = lighting_sample(10.0);
        auto h1 = controller.tick(headroom, candidates);
        assert(h1.kind == QualityDecisionKind::None);
        auto h2 = controller.tick(headroom, candidates);
        assert(h2.kind == QualityDecisionKind::Restore);
        assert(!h2.plan.actions.empty());

        controller.note_action_applied(h2.plan.actions.front(), QualityDecisionKind::Restore, 10.0, 12.0, true);
        assert(controller.active_actions().empty());

        auto stats = controller.effects().find(action);
        assert(stats.has_value());
        assert(stats->samples == 2);
        assert(stats->mean_gain_ms > 2.4 && stats->mean_gain_ms < 2.6);

        const auto state = controller.state();
        assert(state.degrade_actions_applied == 1);
        assert(state.restore_actions_applied == 1);
        assert(state.direction_changes == 1);
    }

    // A quality ladder must degrade in ascending sequence and restore in the
    // exact reverse order. A later step is not allowed to jump ahead merely
    // because its utility score is better.
    {
        AdaptiveQualityController controller{immediate_config()};
        std::vector<QualityActionCandidate> ladder{
            {100, QualityDomain::Lighting, "lighting step 0", 1.0, 0.08, 0.95, 0, true, false, 0},
            {100, QualityDomain::Lighting, "lighting step 1", 3.0, 0.01, 0.95, 0, true, false, 1},
            {100, QualityDomain::Lighting, "lighting step 2", 4.0, 0.01, 0.95, 0, true, false, 2},
        };

        const auto overloaded = lighting_sample(24.0);
        auto d0 = controller.tick(overloaded, ladder);
        assert(d0.kind == QualityDecisionKind::Degrade);
        assert(d0.plan.actions.size() == 1);
        assert(d0.plan.actions.front().sequence == 0);
        controller.note_action_applied(d0.plan.actions.front(), d0.kind, 24.0, 23.0, true);

        auto d1 = controller.tick(overloaded, ladder);
        assert(d1.kind == QualityDecisionKind::Degrade);
        assert(d1.plan.actions.size() == 1);
        assert(d1.plan.actions.front().sequence == 1);
        controller.note_action_applied(d1.plan.actions.front(), d1.kind, 23.0, 20.0, true);

        auto d2 = controller.tick(overloaded, ladder);
        assert(d2.kind == QualityDecisionKind::Degrade);
        assert(d2.plan.actions.size() == 1);
        assert(d2.plan.actions.front().sequence == 2);
        controller.note_action_applied(d2.plan.actions.front(), d2.kind, 20.0, 16.0, true);
        assert(controller.active_actions().size() == 3);

        const auto headroom = lighting_sample(8.0);
        auto r2 = controller.tick(headroom, ladder);
        assert(r2.kind == QualityDecisionKind::Restore);
        assert(r2.plan.actions.size() == 1);
        assert(r2.plan.actions.front().sequence == 2);
        controller.note_action_applied(r2.plan.actions.front(), r2.kind, 8.0, 12.0, true);

        auto r1 = controller.tick(headroom, ladder);
        assert(r1.kind == QualityDecisionKind::Restore);
        assert(r1.plan.actions.front().sequence == 1);
        controller.note_action_applied(r1.plan.actions.front(), r1.kind, 8.0, 11.0, true);

        auto r0 = controller.tick(headroom, ladder);
        assert(r0.kind == QualityDecisionKind::Restore);
        assert(r0.plan.actions.front().sequence == 0);
        controller.note_action_applied(r0.plan.actions.front(), r0.kind, 8.0, 9.0, true);
        assert(controller.active_actions().empty());
    }

    // Noise around the target must not chatter quality. After a real overload,
    // settling + hold time prevent an immediate reversal.
    {
        AdaptiveQualityControllerConfig cfg{};
        cfg.overload_samples_required = 3;
        cfg.headroom_samples_required = 3;
        cfg.settle_samples_after_change = 2;
        cfg.minimum_hold_samples_after_degrade = 5;
        cfg.frame_ewma_alpha = 0.40;
        cfg.overload_margin_ms = 0.20;
        cfg.extra_restore_headroom_ms = 0.20;
        cfg.optimizer.restoration_headroom_ms = 1.0;
        cfg.optimizer.minimum_gain_ms = 0.01;
        AdaptiveQualityController controller{cfg};

        std::vector<QualityActionCandidate> candidates{
            {7, QualityDomain::Lighting, "far lights", 2.0, 0.05, 0.95, 0, true, false, 0},
        };

        for (int i = 0; i < 30; ++i) {
            const double ms = (i & 1) ? 16.82 : 16.58;
            assert(controller.tick(lighting_sample(ms), candidates).kind == QualityDecisionKind::None);
        }
        assert(controller.active_actions().empty());

        QualityDecision degrade{};
        for (int i = 0; i < 12 && degrade.kind == QualityDecisionKind::None; ++i) {
            degrade = controller.tick(lighting_sample(20.0), candidates);
        }
        assert(degrade.kind == QualityDecisionKind::Degrade);
        controller.note_action_applied(degrade.plan.actions.front(), degrade.kind, 20.0, 17.8, true);

        // Deep headroom arrives immediately, but the controller must wait for
        // both settling and the minimum hold interval before restoring.
        for (int i = 0; i < 4; ++i) {
            assert(controller.tick(lighting_sample(10.0), candidates).kind == QualityDecisionKind::None);
        }

        QualityDecision restore{};
        for (int i = 0; i < 20 && restore.kind == QualityDecisionKind::None; ++i) {
            restore = controller.tick(lighting_sample(10.0), candidates);
        }
        assert(restore.kind == QualityDecisionKind::Restore);
        controller.note_action_applied(restore.plan.actions.front(), restore.kind, 10.0, 12.0, true);
        assert(controller.state().direction_changes == 1);
    }

    // If a restore is immediately followed by a degrade, the next restore must
    // receive a stronger hold window so target noise cannot create ping-pong.
    {
        auto cfg = immediate_config();
        cfg.minimum_hold_samples_after_degrade = 2;
        AdaptiveQualityController controller{cfg};
        std::vector<QualityActionCandidate> candidates{
            {17, QualityDomain::Lighting, "reversal step", 1.0, 0.05, 0.95, 0, true, false, 0},
        };

        auto d0 = controller.tick(lighting_sample(20.0), candidates);
        assert(d0.kind == QualityDecisionKind::Degrade);
        controller.note_action_applied(d0.plan.actions.front(), d0.kind, 20.0, 18.0, true);

        // Drain the normal hold, then restore.
        for (int i = 0; i < 2; ++i) (void)controller.tick(lighting_sample(10.0), candidates);
        auto r0 = controller.tick(lighting_sample(10.0), candidates);
        assert(r0.kind == QualityDecisionKind::Restore);
        controller.note_action_applied(r0.plan.actions.front(), r0.kind, 10.0, 12.0, true);

        auto d1 = controller.tick(lighting_sample(20.0), candidates);
        assert(d1.kind == QualityDecisionKind::Degrade);
        controller.note_action_applied(d1.plan.actions.front(), d1.kind, 20.0, 18.0, true);
        assert(controller.state().restore_guard_remaining >= 8);
    }

    // A stale learned restore estimate must not create frame-by-frame probe
    // chatter. Probes require sustained deep headroom; if the same step is
    // reversed shortly afterwards it is quarantined before it may be probed
    // again.
    {
        auto cfg = immediate_config();
        cfg.optimizer.restoration_headroom_ms = 0.5;
        cfg.restore_probe_samples_required = 3;
        cfg.restore_probe_headroom_fraction = 0.20;
        cfg.restore_reversal_window_samples = 4;
        cfg.restore_backoff_base_samples = 5;
        cfg.restore_backoff_max_samples = 20;
        AdaptiveQualityController controller{cfg};

        std::vector<QualityActionCandidate> candidates{
            {31, QualityDomain::Lighting, "stale restore", 8.0, 0.05, 0.95, 0, true, false, 0},
        };

        auto degrade = controller.tick(lighting_sample(20.0, 10.0), candidates);
        assert(degrade.kind == QualityDecisionKind::Degrade);
        controller.note_action_applied(degrade.plan.actions.front(), degrade.kind, 20.0, 12.0, true);

        // There is ordinary headroom, but not enough for a normal restore and
        // not enough for the deep-headroom probe escape hatch.
        for (int i = 0; i < 8; ++i) {
            assert(controller.tick(lighting_sample(8.5, 10.0), candidates).kind == QualityDecisionKind::None);
        }

        QualityDecision probe{};
        for (int i = 0; i < 3; ++i) {
            probe = controller.tick(lighting_sample(7.0, 10.0), candidates);
        }
        assert(probe.kind == QualityDecisionKind::Restore);
        assert(controller.state().restore_probes == 1);
        controller.note_action_applied(probe.plan.actions.front(), probe.kind, 7.0, 10.5, true);

        // The probe was too optimistic and is reversed inside the reversal
        // window. That exact action must receive a cooldown.
        auto reverse = controller.tick(lighting_sample(12.0, 10.0), candidates);
        assert(reverse.kind == QualityDecisionKind::Degrade);
        controller.note_action_applied(reverse.plan.actions.front(), reverse.kind, 12.0, 8.0, true);
        assert(controller.state().restore_backoffs == 1);

        for (int i = 0; i < 4; ++i) {
            assert(controller.tick(lighting_sample(7.0, 10.0), candidates).kind == QualityDecisionKind::None);
        }

        // Once the bounded cooldown expires, sustained deep headroom may probe
        // again instead of permanently deadlocking quality recovery.
        QualityDecision retry{};
        for (int i = 0; i < 4 && retry.kind == QualityDecisionKind::None; ++i) {
            retry = controller.tick(lighting_sample(7.0, 10.0), candidates);
        }
        assert(retry.kind == QualityDecisionKind::Restore);
        assert(controller.state().restore_probes == 2);
    }

    std::cout << "adaptive-quality-controller-tests: PASS\n";
    return 0;
}
