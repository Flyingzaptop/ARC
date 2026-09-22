# Fine-grained GPU budgets

Active target-feedback path now searches sample retention from 100% to 1% in one percentage-point increments and PCF from 25 to 1 taps in single-tap increments. Pixel PID output uses the same ranges. Supported normalized sample/ray loops use ceil(original_count * percent / 100), preserving original normalization at 100% and rejecting unsafe patterns. Original loop bounds 2..256 are supported. Eight original rays expose every count 1..8; sixteen expose 1..16. Counts above 100 remain percentage-quantized (256 at 1% executes 3), not an exact-count interface. No extra rays beyond the original bound are invented.

PCF retains the old nine-tap subset at nine, includes the center at every level, and normalizes by actual selected taps. Zero means original 25 taps.

Audit: hardware VRS and compute density remain discrete supported rates; density changes require aligned reconstruction, not just a new number. Mip bias remains half-level steps. The legacy image-validated search still has preset candidates; this change covers the active target-feedback controller and shared runtime transformations. Named samples25/50/75 diagnostic commands remain shortcuts, not limits on live policy values.

Validation: Release probe/worker and native test builds passed. Target feedback tests cover every percentage and every PCF budget. Native GPU oracles cover all eight independent-ray counts, non-preset texture sample percentages, one-tap and nine-tap PCF, neutral execution and restoration. No new FPS/GPU-time claim or long benchmark run for this granularity-only change.
