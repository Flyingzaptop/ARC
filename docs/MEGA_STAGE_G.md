# Mega G — predictive restoration and bounded adaptation

Stages 23/24 consume Stage 18 forecasts and Stage 19 importance through the same
portable capability/evidence contract as Mega F. The initial predictive model is
short-horizon temporal extrapolation, not a pretrained game-specific neural model.

`PredictivePerceptualOptimizer` restores an active action when predicted 2/8-frame
coverage reaches the configured guard, when importance rises, or when current
visibility/identity/capabilities become uncertain. Stale/nonmonotonic frames, missing
direct visibility, changed target generation or unknown Present relevance cannot
authorize further degradation. Raster bounds alone are insufficient.

Only independent, accepted Mega F trials update the positive gain EWMA. Rejections
add cooldown; image damage quarantines the action/target/generation for the session.
No learned value alters image thresholds, synchronization or rollback rules.
Capacity exhaustion abstains on new actions rather than evicting quarantine.
The checkpoint is versioned and scoped to an opaque application/device/driver
context. Import validates every record atomically, including duplicates and numeric
values. Snapshot serialization is a host responsibility; the portable core has no
filesystem or engine dependency. Reset first restores an active action, and cannot
erase an unresolved rollback failure.

## Native combined F/G evidence

The native fixture first accepts a useful SRV mip change. The actual rendered
affected region then grows from 16x16 to 20x20 pixels on a 128x128 target. GPU
readback supplies observed coverage to Stage 18; the generated region motion is
fixture input, not an engine semantic hint to policy.

Measured current coverage is 400/16384 = 2.4414%. Stage 18 forecasts approximately
3.0654% at eight frames, crossing the default 3% restoration guard. Mega G restores
the original SRV while current coverage is still below that threshold. Another GPU
readback verifies the restored mip/state. The independent Python validator
recomputes region coverage and the forecast from raw pixels and checks both the
physical restoration and the accepted-trial count.

This proves the controlled prediction-to-actuation path. It does not prove
prediction calibration or subjective quality across arbitrary games. The next
steps are independent-renderer portability and generic DX12 connection, with
unknown/unsupported capabilities continuing to abstain.
