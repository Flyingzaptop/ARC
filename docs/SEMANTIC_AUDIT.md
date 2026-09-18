# Independent semantic audit (catalog v1)

The observer alone cannot prove fine roles such as temporal history, material
versus arbitrary sampled texture, shadow versus camera depth, or geometry
versus arbitrary shader-read data. Its confidence remains a heuristic score.

This audit checks six **allocation/use families**, not those fine subtypes:
color-output-capable textures; depth attachments; shader storage images;
shader buffers; CPU upload buffers; GPU readback buffers. RenderTarget and
TransientIntermediate map to color-output-capable; ShadowMap and DepthBuffer
map to depth attachments; StorageBuffer and GeometryBuffer map to shader
buffers. The uncollapsed 6-by-12 confusion matrix and every prediction remain
in the report so subtype ambiguity is visible, not scored as fine-role accuracy.

Ground truth comes from independently inspected allocation owners in Wicked
`0b4dd9ebe0025a4a6d8f17c52c943c40d96d62a7`:

- `wiRenderPath3D.cpp`, `ResizeBuffers`: main/ID/distortion/postprocess targets,
  depth attachments, compute scene/depth copies, GUI blur and debug UAVs.
  `rtSceneCopy_tmp` has target capability for aliasing and is classified at that
  capability family, not asserted to be an observed color pass.
- `wiScene.cpp`, `Scene::Update`: instance/material/geometry/skinning shader
  data; their explicit UPLOAD staging arrays; streaming feedback and explicit
  READBACK arrays; query readback buffers.
- `wiRenderer.cpp`: depth shadow atlas, transparent color shadow atlas and
  environment depth attachment allocations.

Names are captured in an evaluation-only side channel at native SetName, mapped
by exact equality against `ArcWickedSemanticAudit.h`. Neither names nor truth
families are event payloads or arguments to core inference/controller code.
Predictions are computed first, then joined with truth; only first baseline
observation per active resource ID is counted. Unknown predictions count as
abstentions; both coverage and precision are reported. The catalog and thresholds
are committed before the first audit hardware run; all mismatches are retained.

Predeclared audit gates: >=20 unique sampled resources, >=4 of 6 families,
coverage >=80%, family precision >=90%, explicit label blindness, and no dropped
audit records. Both live labels and unique audit records are capped at 4096;
overflow invalidates the audit. Resource destruction removes live labels.

Observed creation flags are also compared with GetDesc at the independent
SetName hook; any mismatch invalidates the audit. This detects stale native
pointer identities instead of blaming the classifier for corrupted inputs.

## Population witness (acceptance schema 5)

The 128-resource complexity threshold applies to the largest **measured baseline
snapshot**, selected only by live population, not confidence, classification
accuracy, or scene identity. All five baseline and adaptive live populations are
written beside their scene signatures. The evaluator requires the reported peak
and frame ID to match an actual baseline capture. It preserves the end-adaptive
population separately, and rejects an invented peak or a frame mismatch.

This replaces selecting the last scene as the population for the entire suite.
That endpoint fell to 120 once stale swapchain identities were correctly retired;
counting those dead identities was not valid complexity evidence. No numeric
threshold is reduced. If no complete measured baseline snapshot has 128 live
resources, the gate still fails. Existing scene identity, coverage, confidence,
performance and safety gates are unchanged.

This adds independent hardware evidence for the supported family-level claims.
It does not certify fine-subtype accuracy, calibrate probability, or establish
generalization to another renderer. Fine-role inference must remain observer-only
until attribution provides stronger evidence. This audit cannot grant mutation
permission or change the controller's decisions.
