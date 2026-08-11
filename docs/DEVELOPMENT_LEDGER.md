# MCVR development ledger

## 2026-09-22: native error-boundary follow-up repair

Status: source-fixed; automated verification passed; paired client runtime acceptance pending.
Evidence: static root-cause inventory, injected native contracts, RelWithDebInfo `core.dll` build,
and complete CTest on baseline `dadb7ecb9fcc3b333d48bb0f76b906c979e63ae3`.
Corrects: the earlier native failure-boundary source-closure entry, which missed the four cases
below.

The confirmed and repaired cases are:

- `parallelFor` now treats worker creation as part of the exception-safe operation. A launcher
  failure latches cancellation, joins every worker already started while their referenced state is
  alive, then rethrows the original creation error. The production helper is exercised with an
  injected third-launch failure, normal work, and a throwing task; it does not exhaust system
  threads to create the failure.
- `failure::State` publishes the first kind/result in one allocation-free atomic operation before
  locking, copying strings, or logging. Detail storage and logging are best effort and publish a
  separate diagnostic-failure bit. An injected throwing detail recorder proves that the fatal bit
  and original `VkResult` survive and later normal work is still rejected.
- first-cause diagnostics and device-loss cleanup are separate contracts. A later
  `VK_ERROR_DEVICE_LOST` no longer overwrites an ordinary first cause, but monotonically reaches
  device, framework, renderer, queue-idle, FSR teardown, and asynchronous Streamline cleanup
  decisions. Concurrent records, ordinary-fatal-then-loss, global async-loss/local ordinary state,
  and a concurrent idempotent close gate are covered without submitting real GPU work or changing
  TDR.
- JNI string conversion now copies the exact UTF-16 or modified-UTF-8 length into owned storage
  before a Win32/POSIX module lookup. Pending Java exceptions stop ordinary work; borrowed chars
  and local references are released during unwinding. The same helper replaced all direct
  `GetStringChars`/`GetStringUTFChars` uses in first-party `src/core/middleware` and
  `src/core/loading`. The contract covers non-NUL-terminated Unicode data, null, acquisition
  failure, and an exception becoming pending after acquisition. Loaded module handles remain
  borrowed and are not freed.

The MCVR product changeset relative to the fixed baseline, excluding documentation and including
the three untracked product headers, has SHA-256
`D50CFA7F44668D7A948EA630084F507435B332E768E58BB98F0386C2140695E2`. The
RelWithDebInfo native artifact has SHA-256
`95EB80A6F0C99A9B377CD66FC187A76AACD58C1B11D698095FA8BAFB60FCEAD5`. The
targeted contract passed, the complete CTest suite passed 30/30 including four Vulkan GPU tests,
and the paired Radiance JUnit suite was rerun successfully. An earlier CTest invocation after only
building the targeted binary reported thirteen tests as `Not Run`; after `ALL_BUILD` produced the
missing executables, the full suite passed. That first invocation is a build-order observation,
not a product-test failure.

The actual `GetModuleHandleW` lookup was compiled through the full native target but not driven by
a mocked JVM. No Minecraft client, injected post-world fatal, real or controlled GPU device loss,
integrated-server save/stop, or visual flow ran. Third-party SDK internal teardown and driver
behavior remain outside static proof. No artifact was installed, packaged, or published.

## 2026-09-22: single-port-commit history policy correction

Status: adopted as repository-maintenance policy; no product or acceptance claim.
Evidence: explicit user decision and local Git-history verification.
Supersedes: the new paired remediation-commit recommendation in the pre-commit state audit below.

The maintained branch keeps upstream history followed by exactly one signed `Initial port` commit.
Later Radiance/MCVR changes are amended into that commit while preserving its original author and
committer names, emails, timestamps with time zones, message, and upstream parent. This intentionally
replaces the amended commit object and signature; the old SHA and signature bytes are not an
identity requirement. Cross-repository records use a one-way MCVR reference from Radiance and do
not trigger reciprocal SHA backfills or repeated amendments.

Backups are temporary rollback tools rather than permanent history. Before a destructive local or
remote rewrite, retain the smallest verified bundle needed to restore the affected refs. Remove
superseded project-maintenance backup refs after the rewrite is verified and their recoverability
has been exported. A backup needed to recover a future force-push remains until the remote state is
verified. Do not prune reflogs, force garbage collection, or delete backups of uncertain origin.

## 2026-09-22: paired pre-commit state audit

Status: documented; no staging, commit, amend, push, cleanup, product edit, or new validation was
performed by this checkpoint.
Evidence: Git worktree/index/ignored-file inventory, remote/ref comparison, GitHub signature
verification, native-install hash comparison, pinned dependency inspection, and license inventory.

MCVR `develop` and `origin/develop` both point to the valid SSH-signed `Initial port` commit
`b0173ab855d4f693a0a62216ec07197b184490b5`. The unstaged worktree has 68 tracked changed paths and
ten untracked candidate paths: six product/header/shader files and four tests. The index is empty.
All ten untracked files are part of the implemented contracts and must be included in any later
candidate; `bin`, `build-radiance-1.21.1-neoforge`, loose objects and other generated/install
outputs are excluded.

The paired Radiance audit is the normalized status authority for the thirteen findings, the extra
P1-03 and physical-client findings, runtime blind spots, and the corrected Ponder
`eWarnOutOfVRAM` diagnosis. MCVR's native source closure and automatic tests do not establish live
post-world fatal handling, real device-loss behavior, integrated-server save/stop, broad gameplay,
or visual acceptance. P2-02 and the generic UI PT design remain explicitly open; P2-07's composite
reuse does not resolve full-window multi-pipeline memory pressure.

The ignored native and SDK binaries are install outputs, not source-commit content. Core and XeSS
outputs match the files installed into Radiance, while DLSS/Streamline DLLs come from the pinned
DLSS submodule and SHA-256-pinned Streamline 2.14.1 archive. MCVR's `LICENSE.md` states the primary
GPL-3.0 and file-level exceptions. A public binary release still requires the paired distribution
license checklist described in Radiance; local build reproducibility is not redistribution
authorization.

The preferred history is a new paired remediation commit so the signed `Initial port` commit and
external-review comparison remain stable. If an amend is later explicitly selected, preserve exact
author/committer metadata and message, create newly SSH-signed commit objects, retain verified
backup refs/bundles, push with an expected-old-value lease, and keep the old and new snapshot IDs
side by side in the audit.

## 2026-09-22: native failure boundary source closure

Status: implemented; build-verified; automated-verified; paired runtime acceptance pending.
Evidence: complete first-party native source and JNI inventory, Release build, CTest, build-host
Vulkan GPU tests, paired distribution verification, and artifact hashes.

All 61 remaining direct process exits were removed after classification. Initialization failures
throw through a non-sticky initialization boundary, while runtime, invariant, and device-lost
failures publish one synchronized immutable first-failure record. Normal JNI calls reject work
after fatal; `lastFailureDescriptionNative`, loading cleanup, renderer close, and close-before-init
are explicitly allowed. All 182 JNI exports under `src/core/middleware` and `src/core/loading` are
guarded. Repeated close and close-before-initialization are safe.

The audit extended beyond JNI. `parallelFor` captures the first worker exception, stops admitting
new work, joins all workers, and rethrows on the caller. Vulkan validation, Streamline logging and
FidelityFX message callbacks contain exceptions at their C ABI. The DLSS-G asynchronous callback
publishes `VK_ERROR_DEVICE_LOST` globally. Swapchain recreation preserves an existing typed fatal
result. Renderer, framework, device, and early-loading cleanup skip idle waits once device loss is
known. Cleanup uses an allocation-free atomic device-loss result rather than copying the locked
diagnostic snapshot from a destructor. Two-step `Framework` creation retains constructed members during partial initialization,
and a failed singleton constructor can be retried.

The failure-state contract injects initialization, runtime, concurrent-first-failure, post-fatal,
and worker-thread failures; source contracts reject process exits and unguarded JNI exports. The
Release build completed and the paired Radiance build installed the result. The final product-source
snapshot, excluding docs and including untracked product files, is
`4CE216AF5378A526243437EE3C9FD062A7807F7307A0DD60EFCBEE138F6F3137`. Build and installed
`core.dll` share SHA-256 `5AA4C808C2C766270491FF93389D4ECE39350385CEE204B319A8BD8959860FB3`.

The first full CTest run after adding cancellation included a scheduler-sensitive assertion about
how many already-running callbacks completed. That assertion was not part of the error-propagation
contract and was removed; the final full result is 30/30 passing, including four Vulkan GPU tests.
The paired packaged dedicated-server check reached `Done (3.300s)` without publishing Radiance's
nested GAME mod or extracting the native payload. Its Gradle wrapper was interrupted after
readiness because it did not forward `stop`; this is discovery/startup evidence, not graceful
shutdown evidence. No live injected Minecraft failure or actual device loss has been run. Driver
behavior, third-party SDK internal cleanup, Java error presentation, and integrated-server
save/stop remain runtime blind spots. P2-02 and generic UI PT were untouched.

## 2026-09-22: Streamline failure identity and Ponder VRAM diagnosis

Status: reproduced and identified; fallback verified live; memory repair pending.
Evidence: full compatibility packaged-client runtime, native allocation counters, and persistent
Streamline diagnostics.

The NGX compatibility wrapper previously mapped every failed Streamline operation to
`NVSDK_NGX_Result_FAIL_FeatureNotSupported`. A 3840x2054 Ponder reproduction proved that the real
failure was `slEvaluateFeature -> Result::eWarnOutOfVRAM (39)`. Multiple full-window independent
PT/RR pipelines overlapped during resize and a two-scene transition, and native reporting reached
about 13.8 GiB of VRAM on the 16 GiB test card. The current-frame spatial fallback kept the JVM
alive, but the unresolved pressure reduced the instance to about 2-3 FPS.

Streamline errors now retain the failing stage, named/raw result, feature, viewport, frame, and
dimensions in `radiance-streamline.log`. SR/RR feature requirements, including
`maxNumViewports`, are queried at startup. Repeated identical failures are rate limited in the next
build. The native Release build and CTest suite passed 30/30 before runtime deployment. This entry
does not claim that Ponder's full-window resource design is fixed.

## 2026-09-22: native failure containment batch 4, active-path slice

Status: partially implemented; build-verified; automated-verified; deployed through the paired
Radiance packaged client; runtime acceptance pending.
Evidence: static, build, automated, build-host Vulkan GPU tests, and artifact hashes.

Historical intermediate state: the source-closure entry above supersedes the current P1-03 status
and removes the 61 exits that remained in this slice.

Active present, submitted-frame readback and screenshot-fence failures now enter
`Framework::recordFailure` instead of calling `exit()`. An optional NGX-directory failure disables
DLSS cleanly. Missing ray-tracing shader packs and invalid overlay-buffer handles throw through JNI
operations already guarded by `jni::invokeVoid`. A source-level regression contract prevents these
specific active paths from regaining direct process exits.

This does not close P1-03. Sixty-one direct exits remain in Vulkan construction, resource
validation and the older Vulkan framework. They are deliberately retained until every reachable
JNI and worker boundary is classified and protected; blindly replacing them with exceptions could
allow an exception to escape C ABI or a native thread and call `std::terminate`.

The Release build produced `core.dll` and all 30 CTest entries passed, including four Vulkan GPU
tests. Radiance then completed `prepareRuntime build preparePackagedClient`. MCVR `bin/core.dll`
and Radiance's embedded copy share SHA-256
`0C58A958D1AD89C8997861733E7240CDBDF67AD574BBFF6A8C24D26487B1B6D7`. No live injected-failure or
device-lost test has been performed.

## 2026-09-22: external-section and Ponder resource repair batch 3

Status: implemented; build-verified; automated-verified; runtime and visual acceptance pending.
Evidence: static, build, automated, and build-host Vulkan GPU tests.
Applies to: the same uncommitted paired worktrees as batches 1 and 2.

External chunk handles now encode a slot generation. Released slots enter a free list only after
their logical generation is invalidated; new allocations install a fresh `Chunk1`, while old GPU
objects are retained through frame retirement. Queued CPU copies, pending build results and GPU
batch completion all revalidate the captured generation before publication. The stress test cycles
one slot 100,000 times without capacity growth and rejects every stale handle.

Each Ponder scene now owns one composite image and descriptor table per swapchain frame. Stable
draws rebind current inputs and reuse those objects; scene retirement retains their owner until GPU
work is safe. Full-window tracing and the existing crop are unchanged pending an explicit viewport
and visual design.

The full Release build produced `core.dll`, and all 30 CTest entries passed in 3.76 seconds,
including external-handle churn, Ponder resource ownership, shader/camera contracts, failure-state
contracts and four Vulkan GPU tests. No Minecraft client was launched. Real Sable/Aeronautics churn,
Ponder transition/resize behavior and visual framing remain unverified.

## 2026-09-22: camera and dead Flywheel data repair batch 2

Status: implemented; build-verified; automated-verified; runtime and visual acceptance pending.
Evidence: static, build, automated.
Applies to: the same uncommitted paired worktrees as batch 1.

All camera-producing ray paths now call `buildWorldCameraRay`. The helper preserves the established
perspective behavior and uses per-pixel origins with parallel directions for orthographic scenes.
The audit was extended beyond priority/background to both world ray generators, advanced primary,
volumetric light, and the direct-light surface reconstruction path. A source contract prevents any
of these consumers from rebuilding projection rays locally; the normal build also compiled the
shader tree. Ponder alignment still requires visual acceptance.

The unused Flywheel dynamic light-section chain was removed end to end: JNI endpoints, CPU maps
and copies, per-frame GPU buffer allocation/upload, descriptor binding 11, and 18-cube shader
sampling helpers. Per-instance scene lighting and the shader-light inputs remain. The full Release
build produced `core.dll`; JNI coverage, shader packaging, camera-ray, instancing, and failure-state
tests passed. The paired Radiance JUnit suite also passed.

No Minecraft client was launched or artifact deployed. Performance improvement and Create/Flywheel
visual equivalence remain runtime acceptance items.

## 2026-09-22: renderer failure-state repair batch 1

Status: implemented; build-verified; automated-verified; runtime acceptance pending.
Evidence: static, build, automated, and build-host Vulkan GPU tests.
Applies to: uncommitted MCVR worktree based on
`b0173ab855d4f693a0a62216ec07197b184490b5`; paired Radiance worktree based on
`249f9a63bd861a3273d2d43ff6e594bff980c66e`.

Frame acquisition now records acquire success independently from swapchain recreation. Two
out-of-date acquisitions followed by successful recreations return `VK_NOT_READY` without
publishing an image index or semaphore, and the old current context is cleared. Non-transient
acquire and frame-fence failures enter the existing `recordFailure` channel instead of calling
`exit()`.

Streamline feature loading now returns a typed result. An absent runtime satisfies a request to
keep FG disabled, while an enable request remains explicitly unavailable and SDK failures remain
distinct. DLSS SR/RR evaluation now has an explicit output-valid state: failure requests a history
reset, does not copy the undefined output into diagnostics, and throws before depth, motion,
normal, or later render-graph consumers run.

The new `mcvr.failure-state-contract` covers double-out-of-date exhaustion, suboptimal acquisition,
Streamline-absent enable/disable decisions, SDK failure classification, and DLSS output/history
state. A complete Release build produced `core.dll`; all 27 CTest entries passed in 3.75 seconds,
including the existing tessellation, custom-vertex-array, exposure, and framebuffer Vulkan GPU
tests. Radiance's complete JUnit suite also passed.

No Minecraft client was launched and no DLL/JAR was deployed. The remaining direct `exit()` sites
are still under staged review; this entry claims only the acquire and frame-fence slice. The full
cross-repository scope is recorded in Radiance
`docs/audits/2026-09-22-gpt6-pro-code-review-verification.md`.

MCVR is the native renderer paired with the sibling Radiance mod. Product-level decisions and
deferred work are recorded in Radiance `docs/DEVELOPMENT_LEDGER.md` and `docs/ROADMAP.md`; this file
keeps the native half discoverable when MCVR is reviewed on its own.

Investigation records that are not implementation claims:

- `docs/research/NVIDIA_FRAME_GENERATION_AND_NEURAL_RENDERING.md` records Ada MFG, unofficial NR,
  and GUI/background-blur FG findings.
- `docs/research/NATIVE_RENDERER_INVESTIGATIONS.md` records the native side of the proposed
  RenderPearl host migration and the PT/Ponder correctness backlog.

## 2026-09-21: historical dirty-worktree checkpoint

Status: superseded as a repository-state claim; retained as a historical snapshot.
Evidence: static, build, and automated results described below; no independent client acceptance.

> Historical snapshot. Its uncommitted-worktree instructions are superseded by the post-amend
> correction below.

Native work captured by this historical snapshot includes:

- chunk generation/epoch validation, fenced batch publication, and per-frame native scheduling in
  `src/core/middleware/com_radiance_client_proxy_world_ChunkProxy.cpp` and
  `src/core/render/chunks.*`;
- display-resolution HDR camera effects carried through `WorldUBO`, JNI, tone-mapping descriptors,
  and `src/shader/world/tone_mapping/tone_mapping.frag`;
- signed independent U/V fluid spans so the backend can consume either vanilla 4 by 4 UVs or the
  exact values supplied by an actual Veil screen-quad call;
- related denoising, ray-generation, Streamline, and world-prepare changes visible in the
  checkpoint's Git diff.

Evidence must distinguish C++/shader compilation, CTest, real command execution, displayed pixels,
and user acceptance. Radiance owns the Minecraft launch and packaging boundary; MCVR is not tested
as an independently launchable client.

Historical handoff instruction, now superseded by the correction below: before committing, update
this file with the final native file list, CTest count, packaged DLL hash, runtime result, and any
validation messages that remain unresolved.

Workspace organization on 2026-09-21 added shader-specific LF rules in `.gitattributes` and
normalized the two modified `world.rgen` files from mixed CRLF/LF to LF. No files were staged or
committed, and this normalization does not change shader behavior.

## 2026-09-21: Windows generator guard

Status: implemented; build-verified for configuration and generator selection.
Evidence: static and build; no independent client runtime or visual claim.

The complete Windows configuration with both FidelityFX and NRD is supported through a Visual
Studio x64 generator. FidelityFX forces `CMAKE_GENERATOR_PLATFORM=x64`; its leaked cache value is
forwarded by NRD's nested ShaderMake configure, while Ninja rejects generator platform arguments.

`CMakeLists.txt` now rejects that exact Ninja combination before compiler and dependency discovery,
and reports a working Visual Studio configure command. A fresh negative-path configure produced the
intended failure in 121 ms. Radiance's `configureRuntimeDependencies` independently selects or
reuses a Visual Studio generator, and a positive configure completed with `Visual Studio 18 2026`
and x64. The Radiance entrypoint also ignored an externally forced `CMAKE_GENERATOR=Ninja` and kept
the explicit cached Visual Studio generator. Direct builds that disable either FidelityFX
upscaling or NRD are not blocked by this guard.

## 2026-09-21: post-amend repository-state correction

Status: implemented; static Git/remote verification. No new native runtime or visual claim.
Evidence: static Git status, local/remote ref comparison, and commit inspection.

The implementation baseline immediately before the current documentation-maintenance batch is
MCVR `5150670796380bf128fecec551c864f480bb05f9` (`Initial port`). Its local `develop` matched
`origin/develop`, and product source was clean. The native work described by the historical
dirty-worktree checkpoint had been consolidated into that single commit and pushed.

This correction supersedes only the old instruction to update the ledger "before committing" and
the claim that the implementation was still uncommitted. The checkpoint's native file scope,
generator evidence, and evidence-level cautions remain historical records. Documentation changes
made after this verification remain intentionally uncommitted for batch review.

## 2026-09-21: native-crash historical extraction

Status: implemented documentation; no new native runtime or visual claim.
Evidence: static current-source inspection plus complete pagination of five historical Codex tasks.

Radiance now owns the cross-repository
[native crash and runtime synthesis](https://github.com/RecRivenVI/Radiance/blob/develop/docs/research/CRASH_AND_RUNTIME_INVESTIGATION_HISTORY.md).
The extraction preserves the repeated A/B/A evidence for the pipeline-layout ownership token while
keeping the NVIDIA-driver mechanism explicitly unresolved. It also corrects a superseded task
claim: at MCVR `5150670796380bf128fecec551c864f480bb05f9`, a descriptor slot retains its current bound
resource and replaces that owner when rebound; it does not retain every historical generation.
`DynamicGraphicsPipeline` separately retains the pipeline-layout/set-layout ownership token.

The old whole-table E-02 experiment, partial device-loss recovery, transient diagnostic layers,
historic test counts, and old deployment hashes are not current product contracts. Independent
validation findings remain tracked in the Radiance roadmap until controlled attribution and
baseline comparison are performed.

## 2026-09-22: lifecycle acceptance instrumentation and RelWithDebInfo artifact

Status: implemented for isolated acceptance; automated-verified; real client cases pending.
Evidence: source inspection, RelWithDebInfo build, 30/30 CTest including four Vulkan GPU tests, and
paired Radiance packaging checks.

The default-off lifecycle acceptance hook adds a controlled runtime-fatal JNI path and read-only
state reporting for the paired isolated client. It records successful and attempted frame submits,
the injection boundary, post-fatal probe-body admission, and close count. The fatal uses the normal
`FatalError` and JNI boundary, so the Java client receives the same class of propagated native
failure used by production errors. Failure diagnostics and shutdown remain callable after sticky
fatal. No device-lost flag is forged and no real GPU reset is attempted.

The contract test covers the probe counters in addition to the existing deterministic thread
creation failure, worker failure, diagnostic-allocation failure, first-cause/device-loss split,
concurrent publication, borrowed-string, and close-gate cases. The actual Java VM will exercise the
Unicode and null JNI-string path during each enabled lifecycle acceptance launch.

The accepted native artifact was built with the existing Visual Studio RelWithDebInfo configuration
and installed into Radiance before the complete distributable JAR was assembled. The matching
`core.pdb` is retained with the machine-local evidence. JNI header generation and native compilation
must remain sequential. MSVC also reported C1041 when the native target used parallel compiler
processes against one PDB; the accepted rebuild used one job. This is a build-host constraint, not a
runtime acceptance result.

### Paired G0 result

Status: passed through the paired isolated Radiance packaged client; G1/G2 pending; G3 deferred.
Evidence: user gameplay observation, client/integrated-server log, process exit, and native
lifecycle counters. MCVR is not treated as an independently launchable client.

The loaded runtime DLL matched the packaged RelWithDebInfo DLL SHA-256
`06707FC39F0679B78C066A89AB89611AB2AA7B34D029D1A9ABE6255E575AB8C9`. Across normal world
entry, mutation, save/exit, re-entry and final exit, the hook recorded 60,485 attempted and
successful submits, zero injections, no fatal state, no device-lost state, and one close call. The
integrated server logged complete save/stop sequences on both exits and the process returned code
0. The user confirmed persistence of the isolated block/container changes and observed no runtime
or rendering problem. This is G0 evidence only; it does not close controlled-fatal, real
device-lost, FG, or general visual acceptance.

### Paired G1 result

Status: passed through the paired isolated Radiance client after one Java lifecycle-boundary repair;
G2 pending; G3 deferred.
Evidence: controlled fatal, native counters, integrated-server save logs, Streamline shutdown logs,
crash reports, and user-confirmed persistence after normal restart.

Both controlled runs preserved first result `-13` and operation
`lifecycle-acceptance/G1-runtime-fatal`, rejected the post-fatal ordinary probe without entering its
body, and recorded no later submit attempt or success. The first run exposed that Minecraft's fatal
process-exit path bypassed its ordinary close method. Radiance added a common fatal-exit close hook;
no MCVR product change was needed for that finding because native close was already idempotent and
fatal-aware. The repaired run saved all dimensions, called native close once, completed
Streamline/NGX shutdown, and preserved the user's new world mutation after restart. This is not
evidence of real device-loss behavior or recovery.

### Paired G2 result

Status: passed through a separate paired Radiance title-screen process; G3 deferred.
Evidence: native lifecycle counters plus client diagnostics and the existing 30/30 deterministic
CTest result.

After the controlled fatal, diagnostic state remained readable, the ordinary JNI probe was rejected
without entering its body, and first-cause data remained unchanged. Two consecutive native close
calls completed and advanced the diagnostic count to two; the first released live resources and
the second was accepted by the idempotent close gate. Submit attempts and successes after injection
both remained zero. The title-screen scope intentionally makes no save/persistence claim. Real
device loss and asynchronous Streamline failure remain outside this result.

### Final paired lifecycle candidate and evidence index

Status: candidate complete for local history folding; real device-loss G3 remains unperformed.
The authoritative paired evidence is retained in Radiance's ignored
`run/lifecycle-acceptance-20260922/evidence` directory rather than committed as logs or binaries.

G0 and the first G1 run used Radiance JAR SHA-256
`F01F2BE306B41F88C5FA6209A3FCF65A60D85FD6E8F9A008DC68C8171B724675`; repaired G1 and G2
used JAR SHA-256 `B8B2287C16509A71A0991BD94C421B38E874ED7898FA5628B109DDDD48389F86`.
All four runs used native DLL SHA-256
`06707FC39F0679B78C066A89AB89611AB2AA7B34D029D1A9ABE6255E575AB8C9`. The final source
snapshot is `evidence/source-snapshot/post-g1-close-fix`; its normalized MCVR product patch SHA-256
is `7253F8098368158A5A5F52083B8AC9781A19D7368C63A57871CD9ACE5E0AE1AA`. Current product and
test sources, including the four necessary new headers, match that snapshot. Documentation was
updated afterward without changing the tested native product.

G0 and initial G1 remain tied to the original JAR, while repaired G1 and G2 establish the final
candidate's fatal-exit behavior. This does not establish real device-loss behavior, general visual
acceptance, or public binary redistribution readiness.

## 2026-09-22: paired external-section generation review

Status: native implementation unchanged; static paired-contract check complete; named Radiance
runtime smoke passed, with longer churn still pending. No native build or CTest result is claimed
for this follow-up.

Radiance's external-section admission repair retains each encoded native handle unchanged and now
rejects stale work at the Java owner/revision boundary before publication. The corresponding MCVR
allocation, release and build paths were rechecked at baseline
`9ebf73cc57290dbbdc9a6dbdd8f19f5989c45dbd`: handle resolution occurs before CPU preparation and
generation is checked again before native geometry publication; release invalidates that
generation. The existing native churn test remains the supporting automated evidence. No MCVR
product or cross-repository interface change was needed, so unrelated native tests were not rerun.

The full isolated mod set created and separated a Sable structure and completed F3+A without a
visible hole or failure. That is a bounded smoke result, not exhaustive Sable/Aeronautics runtime
acceptance: repeated dirty update, unload, handle reuse and world-switch churn must still confirm
visual convergence and bounded queue, CPU and VRAM behavior with the paired Radiance build. One
separate repeat process encountered a driver-confirmed `VK_ERROR_DEVICE_LOST` shortly after world
entry; the prior and following processes using the same JAR completed their named tests, so no
causal attribution to this scheduling change is made.

## 2026-09-22: thread-closure candidate for UI PT, FG composition and long-run resources

Status: source implementation and automated contracts complete; unified client and visual
acceptance pending. The worktree remains uncommitted. This entry does not supersede earlier failed
or partial runtime evidence.

The generic `UiPathTracingProxy` now owns the native UI-scene entry point, with Ponder as its first
caller. Visible transition scenes publish transformed geometry into one shared `World` and TLAS,
while each view retains separate camera buffers, temporal history, frame contexts and composite
images. The PT extent preserves physical viewport aspect but is capped at 1920 pixels on either
axis and 1920x1080 pixels in area; composition still targets the full physical UI image. Per-frame
composite images and descriptors are reused, and an unchanged combined geometry hash skips entity
and BLAS rebuild. Counters expose MCVR-owned UI PT views, geometry builds and composite images;
they do not cover driver or SDK-private allocations.

Frame generation no longer derives a binary UI mask from final-versus-HUD-less RGB. Tone mapping
starts final alpha at zero, normal blended UI accumulates Porter-Duff source-over coverage while
retaining Minecraft's RGB equation, and Streamline receives that fractional alpha. Full-screen
blur, invert and other background-dependent passes explicitly write full coverage and remain
final-color effects rather than pretend independent foreground colors. Real DLSS-G visual checks
for translucent panels, antialiased text, blur, invert and UI PT remain pending.

The texture-name risk was confirmed: IDs grew monotonically against a fixed 4096 descriptor
capacity. Names 1..4095 now use a checked free-list. Priority and priority-background rays now
publish matching depth, motion, normal/roughness and albedo guides when they replace final color.
Flywheel TLAS transforms combine double-precision render origin and local translation with the
camera before final float conversion, preventing visible transform collapse around 30 million
blocks. Absolute light-grid lookup remains a separate float-precision boundary.

Static recheck of DLSS SR/RR found camera-relative motion already includes absolute camera delta,
previous matrices advance only after successful evaluation, and failed evaluation keeps history
reset pending. Those contracts were retained; visual RR/SR history remains a runtime gate. Veil's
fixed-version translation retains real bool/uint kinds and explicit failures for unsupported
forms; this is not a claim that every Veil/OpenGL feature is translated.

The real device-loss incident remains root-cause unknown. Its first recorded client error was
`vkWaitForFences(frame) = VK_ERROR_DEVICE_LOST`; Windows recorded `nvlddmkm` Event 153, Java kept
the original error, and the integrated server saved all dimensions. Native close completion was
not proven. A default-off `MCVR_DEVICE_LOSS_TRACE=1` fixed-capacity trace now records acquire,
frame-fence, submit, present, chunk/texture upload, reload boundaries and asynchronous Streamline
device loss, then writes best-effort evidence after loss. It does not inject a fault.

RelWithDebInfo core and shader compilation succeeded. The first complete CTest run exposed a stale
generated Ponder JNI header and an obsolete source check that rejected writing the priority depth
guide as though it were reading world occlusion. The generated header was removed, the generic
implementation file now follows its JNI class name, and the check rejects an actual `imageLoad`.
Final complete results and package hashes are recorded in the paired Radiance ledger after the
unified build.

### Final unified build corrections

The paired startup preflight found a real texture-upload retirement defect. When the upload queue
became empty, `flushQueuedUploadImpl` returned before polling the final submitted fence, so its
buffers could remain retained indefinitely. Completed buffers are now polled before that return,
and the reusable staging pool is capped at 64 MiB. A second pass showed that recycling the complete
peak atlas capacity for tiny animated updates still caused roughly gigabyte-per-second host churn;
replacement capacity now derives from bytes actually used in the completed batch. The final menu
run stabilized near 517-524 MiB in MCVR-tracked host allocations, with idle zero-delta intervals and
only bounded animated-texture batches. Driver and Streamline-private allocations are not included.

Normal shutdown then exposed `slFreeResources` returning `eErrorInvalidParameter` for a DLSS/RR
viewport which had received options but had never evaluated. The wrapper now records successful
feature evaluation and frees only resources that Streamline actually created. A rebuilt paired
client reached the menu, ran steadily and exited with Gradle code 0 without that error, device loss
or JVM crash.

The final RelWithDebInfo native build passed all 35 CTest cases, including four Vulkan GPU cases,
JNI coverage and shader compilation. Paired Radiance passed 153 GAME tests and seven bootstrap
tests (two skipped), runtime/distribution checks, packaging and the menu preflight. These are
automatic and startup-level results; exact-artifact world, Ponder, FG, reload and long-churn visual
acceptance remains pending, and the earlier real device loss still lacks a confirmed GPU root cause
or proof of native close completion.

## 2026-09-22: successor handoff correction before manual acceptance

Status: directed source/provenance check; no native edit, build, test rerun, deployment or Git index
change. HEAD and the pre-correction tracked diff match the unified manifest. Build, packaged and
extracted DLL identities match `44F0FCC4517A133B1ED22909EB6D224B4A061173232827860113C541B0F76428`;
the latest retained CTest log has 35 passes, which are historical execution evidence.

The preceding entry's shared-pipeline/TLAS completion claim is corrected: `PonderSceneRenderer`
creates a `WorldPipeline` per view; each has its own world-prepare TLAS. A shared `World`/geometry
cache and reused composites are present, but full-window downscaling and immediate per-scene
execution do not complete viewport or current-frame transition ownership. FG's full-coverage blur
also differs from the unchanged HUD-less background; its experience has not been accepted.

The texture pool bounds names, but immediate integer reuse is not a lifetime proof. Submitted
uploads retain actual images; Java deferred glyph uploads still carry only an ID and can resolve a
new owner after reuse. Recorded RT descriptors require their own retirement/generation proof.
Full gameplay handoff is held for these source/contract gaps. Details and gates are in the paired
[Radiance audit](../../Radiance/docs/audits/2026-09-22-gpt6-pro-code-review-verification.md#2026-09-22-successor-handoff-directed-verification-correction).
Observed file/artifact hashes are preserved under Radiance's unified evidence directory as
`HANDOVER-20260922.json`; they do not retroactively attest originally untracked native source bytes.
No real-device-loss, visual or licensing gate was closed by this check.

## 2026-09-22: recorded texture bindings and shared UI execution remediation

Status: implemented; build-verified; automated-verified; bounded runtime-observed; visual pending.
Applies to the retained dirty MCVR tree above `9ebf73c`, paired with Radiance above `e1e91a2`.
Supersedes the preceding directed handoff source gaps, with the contract/runtime limits below.

### Final design

`TextureBindingSnapshots` gives recorded RT commands immutable descriptor/image/sampler ownership;
texture integer reuse only affects later bindings. Java producer generations in the paired tree
protect work that has not reached native yet. The pool remains 1..4095 with explicit exhaustion.
No release introduces routine device-wide idle.

UI PT now collects both scene owners before tracing, builds one common TLAS and uses one actual
`WorldPipeline` with two view-history slots and six physical descriptor/output contexts. Transient
dispatch resources are shared serially; temporal images and upscaler histories are independent.
Current-frame common transforms, owner-selective primary rays and unrestricted secondary rays
retain transition lighting semantics. Crop projection and output placement use actual UI bounds;
the 1920-longest-side/1920x1080 budget remains explicit. Geometry/topology comparison reuses BLAS
for unchanged or material-only content; frame fences retire replaced pipelines and output images.

The client preflight found and repaired a first-batch initialization gap, replay of consumed upload
queues, three-versus-six SBT indexing and overwritten mapped execution constants. New production
helpers and Vulkan tests cover those exact paths. FG now resets world alpha at GUI entry, retains
fractional coverage, mirrors source-only RGB modulation onto HUD-less and shares the real blur
kernel. Local inversion and nested blur are not mathematically equivalent to ordinary alpha; the
fixed SDK has no public arbitrary generated-frame GUI replay hook. P2-02 stays partially open.

Asynchronous OUT_OF_DATE/SUBOPTIMAL now requests surface/history recheck without permanently failing
FG or clearing an earlier fatal. A separate evaluate-result predicate accepts SDK eWarnOutOfVRAM
only after successful evaluate, preserving output/history and feature retirement; it does not
reinterpret options/state errors. Budget pressure remains a recorded condition, not a solved limit.

### Evidence and deployment boundary

RelWithDebInfo core/shader install and 41/41 CTest cases passed, including five Vulkan GPU tests.
The paired Java results are 164 GAME pass and bootstrap 5 pass/2 skip. JNI/export, package and runtime
checks were run; source scans are supplemental to behavior/GPU tests. Failed/intermediate client
runs are retained individually. Advanced compilation initially failed on a missing camera-helper
include, which was fixed; the subsequent Advanced run instead encountered natural device loss.
Its Java first-error propagation, all-dimension save and native close completed. The GPU cause is
unknown, and no TDR setting, illegal access or synthetic lost flag was used.

Final DLL SHA-256: `EF6EE40675B257DA7AED26FEF812A1D0D09C196AB85C468264017FAF2C4B07D1`.
Matching PDB: `584DA1BFD5725A56503F60401D9F94A14BC99FEEEA7E3BCE12EFC0747A0D93FF`.
Paired Radiance JAR: `FAC591721430B18171A1243BFD31B7F7332D79FC7D0E78C49C8F65A2CCEEBCE5`.
The ignored Radiance evidence directory `build/manual-acceptance/evidence/20260922-ownership-ui-fg`
contains `MANIFEST.txt`, `final-sources.json`/ZIP, per-process identity/captures and `ACCEPTANCE.md`.
These include new/untracked sources and fixed dependency identities; they do not retroactively
prove the previous package's missing untracked-source fingerprint. Native allocation counts exclude
SDK/driver-private allocations, and finite stability does not prove unbounded lifetime correctness.

Detailed findings and remaining visual/contract gates are in the paired
[Radiance audit](../../Radiance/docs/audits/2026-09-22-gpt6-pro-code-review-verification.md#2026-09-22-owner-tickets-shared-ui-pt-and-fg-input-correction).
Public redistribution, Advanced GPU root cause, full generated-frame experience and sustained churn
remain open. No staging, commit, amend, push, tag or binary publication occurred.

### Final paired client observation

The final PID 84892 exercised the complete automatic 135-second in-world sequence, including
two-scene Ponder capture/trace, resize to 3840x2054, close/reopen, client reload, server reload and
chunk refresh. All dimensions saved and native close completed with 3,983 submissions, fatal=false
and deviceLost=false; Gradle exited 0 after 3m12s. One heavy executor remained active, transiently up
to three old/new engines were live, then retirement returned to one. Common batches report one
TLAS build and both current owners. The 4K Ponder VMA allocation interval was 6,701-6,702 MiB and
dropped to 6,011 MiB after close, excluding SDK/driver memory. This is bounded runtime evidence,
not a full-process VRAM/performance improvement claim.

FG inputs now contain actual fractional coverage and equal final/HUD-less RGB on alpha-zero world
pixels. Four capture requests completed; the last crosshair-labelled request retained pause-style
coverage, so it does not establish a distinct crosshair scene. SDK interpolation state changed
with window focus. No generated-display or visual result is claimed. The earlier Advanced loss and
budget-warning failures remain linked to their own artifacts in the manifest.


## 2026-09-22: FG affine replay, weighted blur and Advanced pass parameters

Status: implemented; build-verified; automated-verified; runtime/visual acceptance pending final preflight.
Evidence: source, native GPU regressions, Java/bootstrap/package checks; client capture results below.
Applies to: retained dirty paired worktrees above Radiance e1e91a2 / MCVR 9ebf73c; no Git rewrite.
Supersedes: the previous statement that local inversion necessarily requires opaque UI coverage;
that argument assumed an unchanged HUD-less background. Historical runs and limitations remain.

### Source changes and direct evidence

- Actual Minecraft 1.21.1 / NeoForge 21.1.250 Gui.renderCrosshair uses
  ONE_MINUS_DST_COLOR/ONE_MINUS_SRC_COLOR. The source-generated affine transform
  T(D)=S+(1-2S)D can be applied to both final RGB and HUD-less while preserving UI alpha:
  U'=(1-2S)U+S*A. The ordinary RGB draw and order are unchanged. The same geometry/shader,
  viewport and scissor are replayed using a pre-draw depth/stencil copy so writes/tests do not
  discard the second draw or change main depth. Per-frame scratch is bounded; it costs an
  additional depth/stencil copy for each affine draw, not a GPU idle or opaque screen mask.
- GameRenderer renders the HUD before ClientHooks.drawScreen; Screen.renderBlurredBackground
  calls processBlurEffect. Thus blur after UI is reachable in ordinary pause screens, not just
  hypothetical third-party nesting. The isolated audit mod also calls the actual route after
  fractional text/panel and local inverse, before later UI, including over Ponder.
- For a linear blur L, HUD-less is now L((1-A)*H)/L(1-A), with zero fallback only where
  transmission is zero. A half-float intermediate preweights texels BEFORE linear sampling.
  Normal final-color blur still computes L(F), and its alpha is L(A); no ordering or RGB rewrite.
  Scratch/descriptors are per physical frame; pipeline resources are reused and die with the
  framework. GPU tests cover varying alpha, moving inputs, opaque UI PT-like regions and edges.
- First client candidate 4BB7D6DB... / 5D62DD89... exposed negative foreground residuals after
  blur. Investigation found CPU std::round versus GLSL round disagreement for half-integer
  radii (5*0.5). Host constants now preserve the fraction and the compute shader uses the same
  GPU rounding as the fragment reference. A test invokes the production packing helper at
  radii 2, 2.5 and 1.25. First failure/capture and later artifact remain separate.
- Directed Advanced audit found per-pass execution buffers repeatedly retargeting a shared
  UPDATE_AFTER_BIND descriptor before submission. RT and post-render stages now each keep one
  stable execution-buffer address; existing inline command updates and barriers carry pass
  values in queue order. A real compute test reads three distinct values through one descriptor
  and repeats a frame. This defect is confirmed; the natural device-loss cause is NOT proven.
- The existing default-off fixed 256-entry loss ring records numeric pass fingerprints/frame
  indices. These are CPU recording breadcrumbs, not GPU-completed checkpoint evidence.

### Evidence and remaining boundaries

Evidence root (non-portable): Radiance/build/manual-acceptance/evidence/20260922-fg-boundaries-advanced.
See ADVANCED-INVESTIGATION.md, inherited-evidence.json, source manifests, native-build*.log,
ctest-affected.log, ctest-final-affected.log, java-test-results.json, capture checks and final MANIFEST.
This run executed 19 selected Java tests, bootstrap 5 passed/2 skipped, and affected native tests
including actual FG/framebuffer/execution-buffer GPU cases. Old 164/41 counts are historical,
not claimed as a fresh whole-suite run. Native RelWithDebInfo + INSTALL and paired package gates
were executed. No cross-repository JNI signature changed.

The Advanced PID 61436 has first-error propagation, all-three-dimension saves and native-close
return evidence; this corrects any implication that G3 has no real evidence. The failed artifact
and configuration differ from the later 135-second successful Vanilla run. No complete exact
preflight9 source snapshot is fabricated. GPU faulting command/address, SDK-private pressure
and all asynchronous GPU retirement remain unproven. No Advanced stress repeat, TDR change or
unsafe injection is part of this acceptance.

The fixed Streamline 2.14.1 public contract does not expose a post-generated-frame GUI replay
callback in this integration. The implemented background transforms modify the tagged scene
inputs; real-frame algebra and captures do not prove that SDK interpolation keeps a moving
local inverse perfectly anchored, blur edges artifact-free, or every generated pixel equivalent
to re-executing the effect on a generated scene. Those dynamic checks remain user acceptance;
no background freeze, hard edge or experience downgrade is pre-approved. Public binary license
clearance and unrelated G3/root-cause boundaries remain open. All prior texture ownership,
shared UI PT, resource-type isolation and section admission work is retained.


### Additional capture correction and new Vanilla failure (same work session)

Candidate 84037426... / 3190953E... passed the weighted-blur inputs but its returned-world capture
had three hotbar pixels with invalid reconstructed foreground (minimum -0.0718). A direct contract
check found that blend-disabled GUI draws copied RGB without blending while leaving fractional
fragment alpha in the coverage channel. ShaderTranslator now wraps the actual fragment main and
uses a native push constant to force opaque coverage for default-target, blend-disabled draws.
Original RGB/discard/depth testing remain; blended/custom-framebuffer draws set the flag to zero.
The new ShaderCoverageGpuTest compiles BOTH real vanilla and external translations, executes the
native Vulkan harness and verifies discard/RGB/alpha over flag transitions 0 -> 1 -> 0. It passed.
This does not assert that every unsupported third-party fragment layout has an equivalent capture.

Candidate3 JAR 6FB7F409FDA2461057C00296F0CC3576E09AEC555B5B0BE328698912C9E9E1B1 /
DLL FFD37BD714920E619F9BF8FBD4C72180E82477775BB7716260C697976DF8EE53 passed the
88-second preflight4 (PID50564, 21:03:03..21:04:31, 2727 successful main submissions).
All six FG input captures passed foreground bounds at 2/255; zero-alpha world pixels matched
HUD-less RGB exactly. Ordinary pause, fractional UI -> inverse -> blur -> later UI, Ponder and
transition were reached. The SDK reported window-not-focused and disabled interpolation; these
are input-path/runtime observations, NOT generated-display acceptance. All dimensions saved and
native close completed once. 63 selected GAME tests passed; bootstrap5 passed/2 skipped, and five
native targeted tests passed before this run. Native GPU cases are not display-frame observations.

The same candidate3 in preflight5 with FG OFF then suffered natural DEVICE_LOST at21:06:02,
six seconds after world-ready, BEFORE any capture/Ponder/reload/resize. The first error was
vkQueueSubmit(chunk build)=-4. 123 Event153 entries span21:05:58.669..21:06:02.009, GPUID100.
All dimensions saved; original chunk-submit cause persisted across native close, completed21:06:03.
339 main-submit attempts/338 successes include an unwanted subsequent frame submission after fatal.
The process died before module sampling: its PID/live-module enumeration is unavailable; do not
reuse preflight4's loaded-module observation as if it belonged to this failed process. Deployed
candidate3 identity, source snapshot, launch log, crash report, new trace sections and Windows XML
are retained in the evidence root/preflight5. The old Advanced trace sections remain separately.

The confirmed post-fatal continuation is repaired by runCheckedStage at upload/world-module
boundaries and a fatal recheck before final/readback submissions. Its behavior regression uses a
callee that RECORDS an error and returns normally; subsequent callbacks are rejected and first
cause preserved, without a real GPU loss injection. This is a failure-boundary repair, not the GPU
root cause. The latter remains unknown; no extra speculative allocator/driver/shader setting fix
was applied. No further world run is included in this handoff's validated scope.

The final package below supersedes candidate3 after this guard. Its normal rendering shader changes
are the same, but candidate3's gameplay results are not relabelled as final-artifact runtime results.
Only separate final build/regression/menu preflight is claimed. Unified manual WORLD acceptance is
paused by the new natural Vanilla fault; the previous Advanced-only exclusion is insufficient.
The matrix remains available, with paused portions explicit. No FG visual downgrade is approved.

### Final native artifact and evidence boundary

Final RelWithDebInfo DLL: `A2356F8A1517A051A1502F460E1AB31C01F68B9E1D257F00AD0378E58D51873F`;
PDB: `5DE50AC3D02F6C45F63261423ECA63FB1EA9B0CDBDC0FF0F0BB0DE9809E03622`.
Paired Radiance JAR: `432BCDAC53446B92247ABA39E9C977578ADF7547F266064601DD0B479BA02DD6`.
Native build8/9, including shader compilation and INSTALL, passed. Stage-guard tests passed 7/7
(framebuffer GPU, JNI, coverage, loss trace, failure state, FG GPU, execution-buffer GPU). After
the final WorldPrepare/RT/chunk call-site guards, the directly affected failure-state/JNI tests
passed 2/2. No full native-suite rerun is claimed. Radiance packaging and runtime gates passed;
its ledger records the 63 selected GAME tests, bootstrap5/2 and separate later GPU-test rerun.

PID88792 loaded the final DLL from the isolated runtime cache; build/install/embedded/extracted
hashes match. Menu/load/normal-close preflight observed title 21:21:12..21:21:37, 19776 successful
main submissions, no fatal or lost state, close exactly once and exit0. It did not enter a world.
Candidate3's six valid captures and its separate natural loss are not counted as this DLL's world
acceptance. The post-fatal guard is behavior-tested but the underlying GPU cause remains unknown.

Evidence is under the sibling Radiance `build/manual-acceptance/evidence/20260922-fg-boundaries-advanced`
(non-portable), indexed by `MANIFEST.txt`, `final-sources.json`, `artifact-identity.json` and
`ACCEPTANCE.md`. Exact tracked/untracked source ZIP plus ignored dependency/input fingerprints are
retained. Product manifest `5BA1C61BB02F5CAD7A8FF10974575AF9C0CD25792EC155D76C40FB643B5B98EC`
matches the preflighted stage-guard snapshot; final edits after that are documentation only.
The fixed-capacity loss tracker stays default-off in product code; isolated launchers explicitly
enable evidence collection. No unsafe loss injection, TDR modification or speculative driver
workaround was used. World acceptance is on hold, while final menu-only checks are available.
Generated-display visuals, complete G3 and public binary authorization remain unclosed. No Git
mutation or evidence deletion was performed.

## 2026-09-22 first GPU fault: directed repairs and capture boundary

Status: implemented/build-verified/automated-verified for the named repairs;
GPU root cause investigating, world acceptance paused. Paired Radiance evidence:
`build/manual-acceptance/evidence/20260922-first-gpu-fault` (local/ignored).
See its EXPERIMENTS.md and Radiance dated review's directed-investigation correction.
This entry does not replace older G0/G1/G2 artifacts or any first-failure evidence.

Product changes in this investigation: enable supported Vulkan13 privateData for
Streamline's observed private-data-slot use; add COLOR_ATTACHMENT_OUTPUT producer
to three post-color handoff/reuse/finalization barriers via post_color_sync.hpp;
propagate vkBeginCommandBuffer failure before recording. No blanket barriers,
per-free GPU idle, feature disable or removal of pipeline-layout keepalive.

Default-off diagnostics now associate command/pass fingerprints, GPU checkpoint
results, bounded EXT fault data, exact shader-module SPIR-V and a fixed-capacity
AS create/destroy/instance/build ring. CPU records never imply GPU completion.
An optional borrowed external-agent ABI permits local Aftermath resource/shader
capture before device creation and one bounded post-save/pre-release status wait.
No SDK load/distribution dependency is added to MCVR. The ignored diagnostic agent
uses the installed signed SDK, serializes capped callback writes, does not enable
extra shader-error reporting, and never waits for faulted GPU idle. External-agent
source, CMake inputs, SDK hash, DLL/PDB and actual loaded identities are archived.
First agent preflight's incorrect literal flags were detected, normally stopped
and corrected to installed enum constants; product safety mask excluded unwanted
automatic checkpoints. No preflight failure is relabeled as successful capture.

Native RelWithDebInfo INSTALL and matched Radiance package/bootstrap/runtime gates
passed per diag1..diag5 logs. Actual selected regressions: diag2 six passed
(framebuffer GPU, new post-color-sync GPU, JNI coverage, shaders, loss trace,
failure-state); diag4 three passed; final diag5 five passed(framebuffer GPU,
post-color-sync GPU, JNI coverage, loss trace, failure-state). No final whole-suite
rerun is claimed. The production barrier helper is tested by a real color-attachment
write/readback under synchronization validation. Bounded fault-result truncation,
64-bit ownership ring wrap and absent-agent behavior have direct tests. The external
capture wait's fake-clock test covers finished/failed/error/timeout without GPU loss.
A real JVM agent initialization/unload smoke also passed, separately from client use.

Core validation's privateData VUID04564 and the application LDR handoff hazard
vanished on actual-device retest. SDK-private nv.ngx.dlssd clear/fill SyncVal hazards
remain unclassified; no speculative synchronization patch was applied to them.
Inspected AS ownership, frame-retained builders, build-batch inputs, texture snapshot
retention and upload retirement did not establish the first fault's cause.

The installed VVL AS-build checker was independently disproven for a legal retire
sequence: destroying one separate unused AS corrupts its address registry, then it
rewrites256 live TLAS references. Core and AS-checker-disabled GPU-AV controls each
pass four unchanged-input rounds. Original app AS warnings and the instrumented E3
fault are retained with this confounder; no application-UAF conclusion is drawn.
Selective shader-only GPU-AV ran52s clean but at1-6fps. Actual resolution was2560x1440,
not the initially planned720p. A requested10s audit source had not reached its JAR;
the observer stopped it, and the external audit build/deploy was explicitly corrected.

Normal-config E6 PID36856 (diag4) still lost the device around5s after world entry,
no VVL/trace, Vanilla PT/RR Balanced/FG off/Reflex1/8chunks, before UI PT or actions.
First error survived; all dimensions saved; native close1 returned;352 submissions
remained352. This is genuine save/close evidence, not a solved GPU fault. Aftermath
shader-debug E7b PID71588 (diag5) ran20s with886 submissions and normal close, but
changed compilation/timing and cannot establish normal stability. Resource-only
capture and subsequent findings are indexed separately in the evidence manifest.

Diag5 main DLL `D0DDEDB31C65BDAE6E7C160BF04E27614DDF141B785BBC054FA20CDE5CCDC9D5`,
PDB `D5566E0D2AF71E1FDE0F8531D427A245A51DA1AF2A81BC7BAE0507A94007B091`;
Radiance JAR `FE6E10F0FE731E919C9B6FDFB548613931626205816C3F5B696EB4CD57D4D455`.
Raw and normalized tracked/untracked source manifests plus ignored build inputs
are in diag5/diag5b/diag5c snapshots; only external diagnostic source varies there.
Product manifest `A15B404E44FAA61DDCB283E8467D880FCCB4B745D46830FD02253D4D6DE3E3F1`.
This is an investigation package, not restored unified world acceptance. No staging,
commit, push, production-data access or evidence deletion. Public DLL license gates,
FG generated-display visuals and complete G3 remain open.

## 2026-09-23 follow-up: diagnostic-device correction and normal world control

Diag6 explicitly queries/enables VkPhysicalDeviceDiagnosticsConfigFeaturesNV when the
external capture agent requests configuration, and rejects unsupported requested capture.
The normal/default-off branch is unchanged. This corrects an incompletely specified
local diagnostic device chain, not the original GPU cause. The earlier NVIDIA example
alone was insufficient; the Vulkan feature contract is recorded in EXPERIMENTS.md.
Native INSTALL, selected CTest5/5(post-color GPU, JNI, shaders, loss trace, failure-state)
and matched bootstrap/package/runtime/Maven-development checks passed after this change.

Final investigation DLL `78AD12E7F816B94564924E20C65E57F3D42D14DCFE2A93233FF1EBA9A6B4B192`,
PDB `46F68F42CC2490E765E40814B004539DA0F20C8E45DD6A42C162B1E85EB0C93C`,
JAR `3B735ECF7C8DA943EF3A94235F4C6194AD0180DD759BF55F40A72371FDA2164C`.
Diag6 product manifest `8815A38FE5D298056838278BAE81B526FD26FAE5F57FE7A1B970337010FB0F4F`;
source ZIP `CD45EC30E04C04B955B6C11CF76632F7146AE74F30C4821EB667985909608EF7`.
Complete exact/normalized source manifests retain necessary untracked files and ignored
build/agent inputs. Later maintained-record edits do not change the product digest.
Aftermath is absent from the JAR; the local SDK's existing DLL remains outside distribution.

Configured resource-only capture PID36448: world00:02:40..00:03:00,811 successful
submissions,save/close1,exit0. Normal PID60552:00:04:53..00:05:13,768 successful
submissions,save/close1,exit0, no capture/validation module loaded. Both actual DLLs match
build/embedded/extracted bytes. Both process windows have zero System nvlddmkm events.
No fault dump was generated, so GPU fault mapping and the new real-fault capture wait
remain unexercised. The normal run exceeds the earlier five-second entry phase but does
not establish a causal repair or persistent stability. E6's real loss remains valid.

World acceptance remains held pending a useful unvalidated first-fault capture/invariant
and a corresponding repair or adequately scoped explanation. The prepared next capture,
exact missing evidence and SDK-isolation follow-up are in EXPERIMENTS.md/ACCEPTANCE.md;
no repeated blind crash loop or SDK-disable product workaround was introduced. All inherited
fixes and public-binary/FG/G3 gates remain. No staging, commit, push or evidence deletion.

Successive isolated runs used matching settings but an evolving Minecraft/Sable save;
there is no claim of byte-identical world-state A/B. The stopped post-E11 save is archived
once in Radiance evidence for future cloned-state controls. It cannot recover pre-E6 state
or establish/exclude a Sable cause. Do not omit this confounder when interpreting timing.


## 2026-09-23: Bounded local closeout and acceptance checkpoint

Status: implemented; build-verified; automated-verified; natural device loss still observed.
Evidence: static, build, automated, seven GPU fixtures and one failed client with save/close evidence.
Applies to cumulative candidates over `9ebf73cc57290dbbdc9a6dbdd8f19f5989c45dbd` (122 paths).
Supersedes the previous requirement to establish first GPU cause before local source checkpointing;
historical runtime results and failures are unchanged. See Radiance's dated review closeout for
the canonical paired thirteen-item and expanded-scope table.

`render/upload_retirement.hpp` now controls the production texture retirement loop. Failed polls
stop the caller immediately while failed/unpolled batches remain owned; upload-submit failure
propagates immediately too. Previously recording failure could return to additional upload work
before an outer stage guard. `vulkan/command_result.hpp` checks begin/end/reset, preserving the
first error and preventing chained recording/submission after a failed prerequisite. End/reset
previously ignored VkResult. These are confirmed post-error defects, not an established GPU cause.

`pending_uploads_test` calls the actual retirement helper with success/not-ready/lost batches and
checks ownership, poll/recycle counts and rejected second calls. `failure_state_contract_test`
injects all three command-operation failures and tests no continuation, original cause and sticky
rejection. No real GPU fault is fabricated; test reset applies only to fake operations. Final
RelWithDebInfo INSTALL/shaders passed; CTest 43/43 passed including seven GPU cases. Java165 and
bootstrap5/2-skipped plus package/runtime/Maven-purpose checks passed in the paired repository.
Source-scan contracts are supplementary, not exhaustive behavioral proof. No additional idle,
unconditional barrier, resource keepalive or disabled-feature workaround was introduced.

Final DLL `9DC8A04445541A5FB72E84F4D48E3ABFA95456DB122AED46C4DDABEC18B56C99`, PDB
`6FE66854C3EBB1C9DAE3B43321456F304A9399A91DF560BBB6A9B88093DE6916`, JAR
`CBDEBEB8EB8011831F04C06920D54531D8092CFAC45E507903C9073DB94D5451`. Build/embedded/extracted/PID30596 loaded DLL hashes agree.
Non-portable evidence is in paired Radiance's
`build/manual-acceptance/evidence/20260923-local-closeout`: product/final source manifests,
exact source archive, dependency/runtime/shader hashes, build/test logs, matched symbols and
`normal-closeout-*` process/events/crash evidence. Product archive SHA-256
`5CD7294FBCB98142F77598AF1B065246ACF84298824948008F76F66E46C2CE88`, normalized product
`07744F338FF6289BA044224C761D0E3CB32F3B852DAC6FA19CDB7AB5E193CC6F`. Records-only changes do not require a rebuild.

The final normal client reached the world at00:38:54 and naturally lost the device near00:39:06
+0800 on2026-09-23, before the20s target. Vanilla PT/RR Balanced/FGoff/Reflex1/8chunks/2560x1440,
driver616.92, no VVL/Aftermath/host trace/Ponder/reload/resize. First detection was frame-fence -4;
Event153 at00:39:04/06 does not identify the causal command. All dimensions saved; before/after
close submissions stayed242/242, closeCalls1, first cause retained; exit1. No retry followed.
No async vendor completion, normal-lifecycle pass, full G3 or long stability is inferred.

Unknown first cause no longer blocks the user's normal manual acceptance or local amend. The
failed case remains paused, with no repeated pressure path requested. Current generated-frame
visuals, shared-view memory/transition churn and gameplay results remain pending. Default-off
external diagnostics, pipeline-layout workaround boundaries and public binary license gate remain.
No new rendering feature, remote update, tag or binary release is part of this checkpoint.

## 2026-09-23: Frozen-package manual feedback and source synchronization scope

Status: runtime-observed; user accepted the limited simple-play observation; specialist acceptance
remains incomplete. Evidence: user feedback, archived client/module/lifecycle logs and process exit.
Applies to the native product in frozen checkpoint `2f62a34e768a07c6dd4fd9e024f4bd7a2148ca35`;
this follow-up changes documentation only. The paired Radiance dated review owns the session index.

The user reported no obvious problem during simple play and ended this observation round. Isolated
manual PID73024 (2026-09-23 01:24:03-01:27:04 +0800) used the preceding entry's exact JAR/DLL.
Its loaded core hash was `9DC8A04445541A5FB72E84F4D48E3ABFA95456DB122AED46C4DDABEC18B56C99`.
New World and Test were two different short world sessions (approximately 10 and 36 seconds).
Each completed all-dimension saves; final native close returned once, submissions remained
15175/15175 across close, fatal=false/deviceLost=false, and process exit was 0. This does not
verify reopening the same save or persistence of particular block/container edits.

Vanilla PT/RR Balanced/model6/Reflex1/FGoff, jitter and SHARC on, view8/simulation32, no resource
pack. Output changed from2560x1440 to3840x2054 during the user session; this is not a pressure-test
pass. No Ponder or generated-FG visual acceptance is inferred. Host loss trace/Aftermath were off.
Evidence is retained in the paired checkout's non-portable
`build/manual-acceptance/evidence/20260923-local-closeout/manual-20260923-012403-334`.

Keep separate: automatic PID30596 lost this same device/package after about12s in-world; manual
PID92452 failed Java initialization before any native submission when early resize/texture release
requested the not-yet-initialized TextureManager. The latter closed native once with no device-loss
state, exit-1. Neither failure is erased by PID73024. First GPU cause, full G3/vendor asynchronous
cleanup, long stability, specialized runtime/visual cases and public binary permission remain open.

No source behavior, artifact, settings or test result changed in this record-only follow-up. No
build/GPU/client test was rerun. Source synchronization is authorized independently of those open
gates; it grants no tag, Release or binary distribution authorization.

## 2026-09-23: Native face rules and bounded priority chunk batches

Status: implemented; automated-verified and GPU-verified for the named fixtures; client/performance
acceptance pending. Evidence: static, build, automated, GPU. Applies to the uncommitted worktree
based on `8208f305a71d0ffa56e761cd7b62c1b667572cb4`, paired with Radiance's uncommitted worktree.

The [paired ledger](../../Radiance/docs/DEVELOPMENT_LEDGER.md#2026-09-23-per-draw-face-rules-and-interaction-aware-chunk-scheduling)
owns the product contract, reference snapshots and comparison protocol. Native material_faces.hpp
and util/material_faces.glsl now share per-geometry FRONT/BACK/CW handling, with uniform hardware
fast paths and a determinant correction for mirrored instances. Entity/chunk/Flywheel/UI PT geometry
carries face flags through AS construction and InstanceAppearance. Both bundled ray pipelines,
including shadow and specialized groups, consume them. No primary-camera geometry removal or
blanket non-opaque policy was introduced. CW bit22 avoids the intermediate bit15 collision with
Flywheel depth ALWAYS; the initial A snapshot/artifact is retained as a superseded candidate.

ChunkBuildTask priority is separate from ordered/immediate publication. Interaction does not wait
for a full batch; background has a 25 ms admission wait and 250 ms aging. Aged background admission receives
one in four slots while interaction is pending; the ordinal survives batch/byte-budget boundaries.
The intermediate rule placing all old work before interaction was corrected and regression-tested
with a thousand old requests and continuous input into one-section batches. Production top-K selection uses
bounded candidate storage. Per-poll CPU/input budgets and aggregate 32 MiB in-flight input plus the
existing fence count bound submission; oversized sections run alone. This does not account for
all AS/SDK/driver memory. Stale input is rejected before packing and again under ownership lock;
existing slot generations, completion gates and resource retirement remain necessary.

Default-off chunk_trace.hpp holds 16384 bounded events and preserves revision/build/TLAS/frame
relationships. Clock brackets support cross-JNI correlation; CPU fence observation is not a GPU
execution timestamp or visual acceptance. No driver reset, unsafe loss injection or global GPU idle
is used by this change. The external audit mod provides opt-in client scenarios.

`Radiance/build/manual-acceptance/evidence/20260923-faces-chunks/ctest-final.log` records 48/48 passed,
including 40 Vulkan rays from the material-face fixture, production selection/admission policy,
trace on/off/overflow/concurrency, and existing lifecycle/handle/texture tests. Old supplemental
source assertions for gl_RayFlagsNoneEXT and189 JNI entries were corrected; exported JNI bodies
and the independent declaration/export comparison remain checked. Builds use RelWithDebInfo.
Final package/reference identities are indexed by the paired evidence `deployment.json`, source
ZIP/JSONs and native dependency manifest. Build, embedded and installed JAR hashes match; actual
loaded DLL and world comparison remain pending. Other-client
GPU use is a pending comparison condition explicitly retained by the user. No measured FPS,
latency or total-VRAM improvement is claimed. Existing first-device-loss cause, broader visuals
and public NVIDIA redistribution permission remain open. No Git history or public artifact changed.

Final B core SHA-256: `CFB6294C5F28FBFC058297B3F8F08151457663A29C0DCC62F1EB4EBE829B6A7F`; paired JAR
`C94D647DE56AC0CF806CFB348411AC52034DA66AA74703D7C19AA315F4B878B1`. RelWithDebInfo build/shader/packaging
logs and matching PDB are retained. Trace-only corrections record first presence at an unchanged
revision and submitted batch payload bytes. These are neither GPU execution timestamps nor total
process VRAM. The standalone GPU fixture uses device-reported scratch alignment and bounded memory
type lookup. No new client run or performance gain is inferred from these automatic checks.

## 2026-09-23: Paired Java startup correction

Status: native unchanged; paired bootstrap correction build/automated-verified.
Radiance PID46548 failed before the menu due to broad SERVICE transformation targets
resolving optional Veil/Sodium types. Loaded core was CFB6294C5F28FBFC058297B3F8F08151457663A29C0DCC62F1EB4EBE829B6A7F;
there is no new device-loss evidence. Radiance narrowed targets to actual GL calls and
rebuilt its JAR as 1CB6A559DC800C9B61C73542901B535EB9EB46EE6BA8C6A901E16E42D8221E60. Embedded native/shader entries were
compared to the prior package and are byte-identical; native tests were not rerun.
Paired source/deployment and retained failure logs are in
`Radiance/build/manual-acceptance/evidence/20260923-faces-chunks`. Runtime/visual and
performance acceptance remain pending; no Git history or binary release changed.

## 2026-09-23: Paired manual feedback, native unchanged

Status: observed face/chunk/Sable cases visually-accepted; six visual reports investigating.
PID68896 loaded core CFB6294C5F28FBFC058297B3F8F08151457663A29C0DCC62F1EB4EBE829B6A7F
with corrected Radiance JAR1CB6A559DC800C9B61C73542901B535EB9EB46EE6BA8C6A901E16E42D8221E60.
Ponder flat-colored transitions, water reflections and crop edges; ground-facing glow
lichen emission/UV; Sable particle light; and missing F3+G fine/red line geometry remain
reported failures. Native UI logs show1pixel viewports, executor replacement and a DLSS-D
maximum-viewport warning; none alone proves the flat-color cause. No product changed.

The paired Radiance manual-feedback ledger owns detailed observations and local evidence
under20260923-faces-chunks/runs/manual-20260923-221257-300. The client and observer vanished
after22:34:14 without exit/native-close evidence. No close command was issued or new GPU
loss established. Last dimension/sublevel save22:33:36 is partial persistence evidence,
not a normal-shutdown pass. No automatic restart or performance improvement claim.

## 2026-09-23: Ponder history retirement, coverage and continuation visibility

Status: implemented; build-verified; automated-verified; visual acceptance pending.
Evidence: PID68896 viewport exhaustion/crop logs, native behavior tests and actual GPU fixtures.
The current face/chunk worktree retains both transition worlds. On rare executor capacity or
parent changes, UI PT waits submitted frame fences before closing old SDK histories; an
unsubmitted same-frame recording is explicitly rejected. Cached geometry transfers to the new
executor. No per-frame global idle, screenshot fallback or layout-keepalive removal was added.
Ponder coverage classifies depth before filtering and removes input jitter before compositing.
Both PT primary loops limit view-owner filtering to the first camera segment; continuation rays
can hit the other transition world. These are confirmed defects, not proof of all water/flat-color
symptoms or the prior GPU-loss cause. Existing explicit captures now include both views and can
capture main-world material guides, bounded to8 evaluations and disabled without request files.

RelWithDebInfo INSTALL and shaders passed; CTest50/50 includes the production coverage shader,
UI-owner helper GPU cases and safe history-retirement behavior. Paired GAME177, bootstrap8/2skip,
complete JAR/runtime/Maven artifact gates passed. Retained evidence and PDB:
`Radiance/build/manual-acceptance/evidence/20260923-faces-chunks/artifacts/visual-feedback/B`.
JAR F155373DDBF3E69589386D25862C5B67646C820E63BC13B345FCE85C431CAC22; core DEFA112156C982207A1C9600FEEFE2E63818682F22866EF3C577C6FEEE30559A.
Snapshot `B-visual-feedback-source.zip/json` includes necessary untracked files. UI appearance,
rare-resize stall cost and long runtime remain pending; no Git operations/public binary release.
Radiance also corrects particle ambient sampling and moving content viewport; its ledger owns
the six-report status, including the Sable F3+G explanation and unresolved ground lichen.

Startup observation for the visual-feedback package: PID92456, supervised independently by
PID74100, completed pipeline warmup/resource loading and resumed responsive2560x1440
presentation by23:14:17 local. Actual extracted/loaded core matched
DEFA112156C982207A1C9600FEEFE2E63818682F22866EF3C577C6FEEE30559A.
Run `manual-20260923-231305-345/startup-identity.json` records this startup-only evidence;
world, Ponder and particle visuals remain unaccepted. Existing unsupported Veil shader/layout
messages are not new fatal evidence or proof those optional features work. No client was stopped.

## 2026-09-23: Ponder PT archived by the paired Java entry

Status: native product unchanged in this sub-batch; Ponder PT archived, paired raster implementation
build/automated-verified. The user superseded the mutual PT transition requirement with default
Ponder rendering. Radiance unregistered both PT hooks and retained native service, resource guards,
shaders and tests as dormant implementation. No layout-lifetime workaround or world face/shader
protection was removed. The [Radiance archive](../../Radiance/docs/history/ponder-pt-2026-09-23.md)
and its development ledger own the activation inventory and remaining visual boundaries.

Create configuration failed in PID92456 at contextless GL11.glDisable(STENCIL_TEST), not a proven
GPU device loss. Radiance now routes supported raw capability toggles through existing Vulkan
state APIs and preserves default block/fluid shading in Catnip raster previews. No JNI ABI or
native product edit was necessary. GAME180 / bootstrap8 pass2skip and packaging passed; previous
CTest50/50 was not rerun for this Java-only change. First two real preflights completed original
Ponder/config rendering, forward/back, all-dimension saves and native close, exit0; their JAR was
323E72F94DED4465BDE274484E14788A28FF3CA2819CF6E43D56D3A641D208A3.
Final JAR is9B13C9E8CFAED8F861F4526DB84B902DC20AC595AE1DCF68C262547FF25146F4, same core
DEFA112156C982207A1C9600FEEFE2E63818682F22866EF3C577C6FEEE30559A and retained matching PDB.
Evidence/snapshots are under Radiance's `20260923-faces-chunks` root; final runtime outcome is
recorded there and in the paired ledger. Lichen missing pixels and intermittent raster widget
icons remain unconfirmed causes; Ponder PT crop/reflection failures are archived, not repaired.
Sable particle/debug/structure observations are user-accepted in their stated earlier package.
No staged changes, commits, deployment to production or public binary distribution.

Final pair PID38908 (`raster-20260923-235016-497`) completed the same47-second real-world/config/
Ponder forward-back smoke using final9B13C9E8 JAR and DEFA1121 DLL. All dimensions/sublevels saved,
native close completed once with fatal/deviceLost=false, exit0. Captured icons rendered in this
run; agent observation does not close user visual or long-stability acceptance.

Manual deployment/startup observation: the final pair is deployed to the preserved `manual-2`
isolated world/settings. Independent supervisor63652 launched PID46312 at23:55:37;
resource loading completed by23:56:02 and the responsive window loaded the exact DEFA1121 core.
`manual-20260923-235537-286/startup-identity.json` fixes package/path evidence. Automatic Catnip
smoke and stop are explicitly disabled for this user run. User interaction/visual results remain
pending. This appended startup record is documentation-only after the final product snapshot.

## 2026-09-24: Ponder retirement versus independent native fixes

Status: investigating; no native source/build/deployment change.
Evidence: static inspection of the dirty `8208f30` tree, historical `b0173ab` differences and
existing test implementations; no newly executed GPU or performance test.
Supersedes: no historical validation fact.

The paired [raster/Ponder assessment](../../Radiance/docs/research/RASTER_PARITY_AND_PONDER_RETIREMENT.md)
documents the exact inspection fingerprint and proposed disposition. Ordinary submissions still
include `uiPtCommandBuffer`; cross-view image aliasing does not reduce main-world allocations
with one view. Scene overrides, per-view histories and UI-owner predicates are Ponder/UI-only
specializations to isolate, not demonstrated main-world optimizations. Texture snapshot ownership,
producer generations, last-upload retirement, bounded staging reuse, stable pass-parameter
descriptors, synchronization/fatal guards and layout keepalive retain independent contracts.

Do not revert shared files wholesale or count correctness repairs as measured speedups. The
proposed GL/Vulkan comparison must exercise production translation with matched inputs and keep
main-world PT/FG output differences separate. Current client and artifacts remain unchanged;
runtime equivalence, cleanup implementation and measured effects remain unverified by this entry.

## 2026-09-24: Lazy archived UI commands and Simulated material/diagram contracts

Status: implemented, build/automated-verified, bounded client observations; user visuals pending.
This implements part of the preceding authorized retirement recommendation. Ponder PT stays
archived. No wholesale shared-renderer rollback or new main-world performance claim is made.

`DeferredFrameCommands` separates retained command storage from active recording. Main frames
allocate/begin/end/submit no UI PT buffer unless the archived UI executor requests it. Its recording
still precedes overlays; fence retirement, split readback and failed begin behavior are retained.
The behavioral test covers 500 unused frames, shared storage, one begin per frame and begin failure.

`diagram_math.glsl` restores the original eight-level interpolated fade output rather than a binary
dither decision, exact UNORM8 Bayer thresholds and bottom-up logical UI coordinates. Directional
depth-outline sampling uses the corresponding Y direction. Physical-resolution diagram rendering
remains intentional. `surface_overlay.glsl` unifies stress and hurt as surface-albedo recoloring,
without converting stress to opacity/emission. Both PT packs have a lock priority hit group;
`priority_coverage.glsl` applies the original 0.1 cutout to ordinary lock surfaces in both any-hit
and closest-hit, while retaining fractional text alpha. The first threshold-only implementation
was corrected before final delivery because it incorrectly retained fractional lock edges.

Evidence in sibling Radiance `build/manual-acceptance/evidence/20260924-raster-simulated/`:
RelWithDebInfo INSTALL/affected test build; `ctest-priority-final.log` selected 14/14 passed;
GPU sweep of 263,168 diagram/stress/hurt/priority samples including exact cutout equality and
fractional text; ten affected Vanilla/Advanced hit shaders compiled using preset defaults.
This is bounded behavior/compilation evidence, not all-mode runtime or visual acceptance.

Final native SHA-256 `D9257915FBAA5E69F7C36746173BC95E9D78F9FF4E633137C29673A7C34828A0`,
matching PDB and complete source snapshot are retained there. The final build relinked dependency
outputs; do not use the earlier D34B76C8 native identity for it. The paired Radiance ledger owns
JAR/deployment/client outcome indexing and original Simulated producer semantics. Temporary probe
errors and the initial Java Mixin loading failure are retained there, not classified as native
GPU failures. Existing pipeline-layout workaround evidence and historical device-loss/permission
boundaries are unchanged. No staging, commit, push or public binary release occurred.

Final paired client evidence: Radiance `20260924-raster-simulated/RESULTS.json`, PID3896, roughly
79 seconds in-world with raster diagram/Create/Ponder and captured staff geometry. The final DLL
was actually loaded; all dimensions saved, native close completed with no fatal/lost state, and
exit was 0. This is bounded runtime evidence, not Advanced, stress-spring, mirror or user visual
acceptance. `MANIFEST.txt` and `ACCEPTANCE.md` index the final isolated manual entry and limitations.

Paired packaging update, 2026-09-24: Radiance/audit moved to NeoForge 21.1.251 and added outer
launcher display metadata. MCVR source, shader payload and D9257915 native artifact were reused
without a native rebuild. Radiance's corresponding ledger/evidence entry records isolated menu
PID3528 and the replacement JAR `39F76BC4...`; no native-suite rerun or new world/GPU acceptance
is implied by this Java/packaging maintenance.
## 2026-09-24: Source checkpoint before standalone replay work

Status: source/evidence reconciliation; no new renderer change or runtime acceptance.
Evidence: static, artifact hashes and existing named build/automated/runtime records.

The user authorized folding the accumulated face-state, chunk scheduling, archived UI PT,
diagram/material/priority and deferred-command changes into the single signed `Initial port`.
All native candidate files, including necessary new headers, shaders and tests, match the exact
bytes in Radiance `20260924-neoforge-metadata/final-source.json`; its native payload remains
`D9257915FBAA5E69F7C36746173BC95E9D78F9FF4E633137C29673A7C34828A0`.
The preceding native build, selected 14-test result, shader checks and bounded runtime evidence
are reused with their original dates and scopes, not reported as rerun for this Git operation.

Original parent, author/committer identities, dates/time zones and message are retained, with a
new SSH signature. Source reconciliation and recoverable pre-rewrite bundles are recorded under
`D:\Workspaces\Artifacts\RadianceCommitMaintenance\20260924-amend-push` (non-portable).
Radiance records the resulting native SHA in one direction. No binary publication is authorized.
Outstanding visual, quantitative performance, GPU-causation and licensing limits remain open.
Standalone same-scene replay is subsequent work, not an existing capability proven by this checkpoint.

## 2026-09-24: First static scene capture and standalone MCVR replay

Status: implemented, selected automatic checks passed, bounded capture/replay demonstrated.
Evidence: source, builds, behavior tests, GPU readback and native window inspection.
Applies to: worktree after `4778983778d53084132ce84ed0e567579ed6ed7b`.
Remaining acceptance: representative high-distance scene and controlled performance comparison.

The local-only scene replay tool (subsequently [retired](#2026-09-24-one-shot-scene-replay-retirement)) captures actual main-world
TLAS/BLAS inputs, material/appearance data, textures, uniforms, options and the pipeline graph.
Its native executable rebuilds geometry and invokes the normal rendering modules without a JVM.
Address registration/build metadata are opt-in from process startup; readbacks retain owners
until the actual frame fence, and an event prevents publishing an unexecuted snapshot. An expired
address key initially hid a larger live allocation; the corrected live-range resolver and behavior
test preserve source ownership without globally pinning buffers. Earlier rejected captures remain
evidence. The legacy Exposure slot was excluded after confirming it has no consumer; real tone
mapping/history resources warm up independently in replay.

RelWithDebInfo core/tool/test builds passed. Final selected CTest was 8/8, including actual
`material-faces-gpu`; the new contract test executes address remapping, stale-key resolution,
bounds/overflow and capture-budget checks. Not a full CTest or Java-suite rerun. A transient test
compile failure lacked `<string>`; it was corrected and the new executable was rebuilt before
the final passing run. Full Radiance distributed JAR and runtime/package verification passed.

Evidence root (non-portable): `D:\Workspaces\Artifacts\MCVRSceneReplay\20260924`.
`iteration-3`: 972 instances/unique BLAS, 1,107 geometry slots, 396 buffers, 176 textures,
332,214,176 bytes including same-frame reference output; capture client PID83828 saved all
dimensions and closed native with `fatal=false`, exit0. Captured core `BEB74D66646A063CB9DC7F66D87877BE23D2FBC3C597EB145C35DC5FCB5FD4CD`.
The first standalone attempt safely returned NOT_READY from menu-specific warmup; standalone
setup now recreates the graph before acquiring a frame. `replay-loader-2`, PID82060, loaded core
`681AE67F1D50AA2F9AC8E5B5C73EC9749EE567936800AE8178A30D9110D18D34`, no jvm.dll, rendered
4,497 frames in40.0034 seconds and exited0. Geometry/hand/priority icon corresponded to the
reference during agent inspection. Snapshot ABI stayed unchanged between these generations.

CSV separates completed GPU timestamps from CPU intervals and records focus. Excluding first600
frames, observed CPU p50/p95/p99 were8.471/10.315/11.283ms; GPU8.351/10.132/10.787ms. Focus varied;
these are functional-run data, not a Minecraft speedup claim. UI/ticks/physics/streaming are absent,
textures and geometry are frozen, temporal histories differ, and capture overhead is excluded from
any future comparison. FG, packed emission tables and post-raster world batches are rejected.
512MiB readback limit remains explicit; high-distance capacity is not yet demonstrated.

`iteration-4` holds final source/artifact identity and the full paired JAR. The user requested a
normal repository client for selecting a pressure scene; it has no automatic capture/exit, while
device-loss logging remains enabled and Aftermath remains off. No pressure loop, production/Prism
operation, new commit/push or binary publication was performed for replay work. Existing fault
causation, visual and licensing boundaries remain unchanged.

## 2026-09-24: Bounded pressure capture and Streamline replay lifecycle correction

Status: implemented; build-verified; automated-verified (named contract); runtime-observed.
Evidence: source, RelWithDebInfo builds, behavior test, GPU readbacks and process/module logs.
Applies to: dirty replay work after `4778983778d53084132ce84ed0e567579ed6ed7b`.
Supersedes: the preceding native RR comparison and 512 MiB-only capacity statements.
Remaining acceptance: fixed-condition performance comparison and user visual inspection.

The user's 32-distance capture exceeded512MiB and was safely rejected. The diagnostic buffer
registry also scanned every entry on every allocation above8192 entries, slowing the client;
pruning now occurs once per4096 registrations and registration ends after the one request resolves
owners. Tests cover100,000 registrations, capture-budget parsing/bounds, overflow, address relocation
and stale weak owners. Larger captures require explicit64..8192MiB authorization. Before issuing
copies, preflight counts buffer/texture/reference bytes and checks disk and one quarter of free
physical RAM on Windows. Readbacks require non-device-local HOST_VISIBLE memory; VMA invalidation
is checked. Source owners remain fence-retained and an event still gates snapshot publication.
Ordinary renderer allocation policy and disabled-capture behavior are unchanged.

The standalone loader uploads in64MiB batches and builds64BLAS per batch, retaining staging,
scratch and command/fence owners until actual completion. A failed preparation retains owners
for normal close/error handling. Static JSON/instance/address parsing runs once before timing.
The normal per-frame world preparation, TLAS, PT, RR and post-processing remain active.

History inspection found a separate integration defect: standalone omitted the SDK beginFrame
and Reflex/PCL entry normally supplied by Java. First-frame geometry looked correct, then RR
became overbright or black. Added the real SDK frame entry/markers and nonrepeating-token check;
CSV now records tokens. Optional history captures at frames1/32/256 are off unless explicitly
enabled and disturb timing. This finding invalidates earlier `replay-loader-2` and
`replay-pressure-1`/`replay-pressure-history` full-RR timing/visual equivalence; their normal exit,
absence of JVM and reconstructed geometry remain historical evidence. No Minecraft RR defect
or first-device-loss cause is established by this launcher correction.

Non-portable evidence: `D:\Workspaces\Artifacts\MCVRSceneReplay\20260924`.
`iteration-5/snapshot-1`:13,038 instances/unique BLAS,12,501,568 triangles,5,988 buffers,272 textures,
2,913,864,624 readback bytes under explicit4GiB budget. Capture PID41104 saved all dimensions,
retired the capture during normal close, native fatal=false/deviceLost=false and process exit0.
Capture core `CDE37551C50E95F654FC79C966F2DCA54038696254D20274DEB281BD5B029F4A`; paired
Radiance ledger records the JAR and unchanged saved world/camera. ABI-compatible corrected
replay core `56CF0A565481FB074EB207B978EA7A93E15F4653AC9071F79E4F12D7C3E10F10`, PDB and
source ZIP/manifest are in `replay-pressure-2`; source-manifest SHA-256
`E260784F7C0C0182AE4B8161273BD4D63D687B95559C1B518A52096D93A5BB60` includes necessary new
files with raw/LF hashes. Documentation appended later does not alter those product inputs.

Corrected PID51796 rendered40.007seconds/2,783frames with2,783 unique SDK tokens, no JVM, exit0.
2560x1440, VanillaPT/RR balanced1485x835/presetF,4bounces,SHARC+jitter,Reflex1,FGoff/vsyncoff;
RTX4080SUPER/616.92. Reference/history/final color agreement is agent inspection (RGB MAE2.856/255),
not user visual acceptance. Excluding first10seconds, real CPU intervals p50/p95/p99 were
12.791/20.739/27.058ms, GPU12.368/13.247/14.003ms; only149/2,139 measured frames were focused.
Minecraft still loaded the region and native freezes that captured state, so its86.817ms
pre-capture median cannot establish a controlled speedup. The full-card13,945/13,029MiB peaks
include non-MCVR memory; VMA also contains host allocations and omits SDK allocations.

This round ran core/tool/test builds and scene-replay CTest1/1 after preparation/contract edits;
the later SDK-entry build and corrected real replay add bounded integration evidence. Radiance
distributed/runtime checks and external audit build also passed. Historical8/8 native and other
Java results were not rerun or promoted to this snapshot. Full logs, rejected attempts, separate
source/artifact identities and original saves remain. No staging, commit, push or publication;
first GPU fault, broader runtime/visual acceptance and third-party permission remain open.

## 2026-09-24: Free camera for standalone scene inspection

Status: implemented; build-verified; automated-verified; runtime-observed for scripted movement.
Evidence: camera/transform behavior test and a bounded native GPU run; manual input feel pending.
Applies to: standalone replay work after `4778983778d53084132ce84ed0e567579ed6ed7b`.
Supersedes: fixed-camera-only limitation for interactive (seconds=0) runs; timed defaults unchanged.

The user requested movement and turning within the captured scene. Added `scene_replay_camera.hpp`
and GLFW input in the standalone loop: hold RMB to look, WASD to fly, Space/C vertical, Shift/Ctrl
fast/slow, R to return, Escape to exit. Loss of focus releases capture and suppresses movement.
Camera rotation uses a quaternion, including the pressure snapshot's straight-down initial view.
Position remains double precision. Captured camera effects remain relative to the view matrix.
The renderer receives rebased current TLAS transforms and last-rendered camera-relative instance
history; geometry/BLAS remains immutable. History matrices are indexed by instanceCustomIndex,
matching the shader consumer, rather than relying on TLAS array order. Each frame logs camera
position/orientation. R alone uses the existing GPU-idle history-reset boundary (including SHARC
and RR); ordinary movement has no added idle or pipeline rebuild.

RelWithDebInfo core/replay/test builds passed with existing Options narrowing warnings. Selected
CTest `mcvr.scene-replay-contract` passed1/1, including diagonal speed, view-relative movement,
large world origins, current/previous geometry rebasing, exact restore, vertical orientation and
stall-step bounds. No full native or Java suite rerun was needed for this standalone-only addition.
The explicit, default-off camera smoke entry exercised the same camera update/transform path for
20.0084seconds/1,595frames, PID53140, no JVM, exit0. It rotated, moved39.905blocks, reset once and
returned exactly to captured camera (0.5,513.6199998855591,0.5). This is the eye position; saved
player feet were at Y512. Moved-image RGB MAE61.255/255; restored-image MAE2.218/255 versus the
pre-move image. Agent image inspection agrees with those changes. These are not perceptual quality,
interactive mouse-focus acceptance, benchmark improvement or long-term stability claims.

Non-portable evidence/deployment:
`D:\Workspaces\Artifacts\MCVRSceneReplay\20260924\replay-free-camera`.
Core `E06B2AD5A6C76528EC5E59DFF4622E6F45D5AA7BDEED549A057C64C022DEA29F`, matching PDB,
loaded-module record, test/build logs, source ZIP and manifest
`280EB8F902EA4A615E55213A3F597DDA30BF064957D66DD1AD257DA135DEB113` preserve the tested inputs.
Later documentation edits do not change that product snapshot. `Start-Interactive.ps1` pins the
new core and original scene hashes and clears scripted input/history-capture variables. Existing
fixed-camera benchmark artifacts remain intact. No Minecraft JAR or Prism instance was changed.

Limits: only the captured geometry exists; no collision, streaming, physics or animation. Hands,
entities, fog/fluid state and time remain frozen, so free flight can leave the captured hand behind
or leave the captured region. No resampling/reconstruction of missing world data is implied.
Both repositories remain unstaged/uncommitted; public-binary permission and GPU-fault limits stay open.


## 2026-09-24: Focus-matched Medium snapshot benchmark

Status: runtime-observed, bounded GPU comparison; no new native implementation/build in this
step. Source-normalized verification against the tested free-camera snapshot found only later
documentation differences. Existing uncommitted replay work is retained.

The user's saved Medium world was measured in a separate Radiance run directory, then its actual
PT scene captured and replayed sequentially. See Radiance's
[full conditions and CPU investigation](../../Radiance/docs/DEVELOPMENT_LEDGER.md#2026-09-24-medium-scene-sequential-game-and-native-comparison).
Snapshot8511 instances/BLAS,17112 geometries,11544798 triangles,3552 buffers and264 textures;
scene SHA `120844AACB8B1BF6F366AF167766C667A4688EC8EC7CB61751E14363686A3E82`.
Readback2628051232 bytes,4GiB explicit budget. Manifest published during safe normal shutdown;
CPU capture/write time is excluded from benchmark intervals.

A first native60s run lost focus and is not the matched baseline. Second run PID24520 completed
4386 frames/60.0009s,4386 unique SDK tokens, exit0 without JVM. Focused20-60s contains2997 real
frames: mean13.346ms, p5013.256, p9514.381, p9915.343 (74.93FPS); GPU2996 completed samples
mean13.310ms, p9514.154, p9914.744. Capturing game PID36400's focused90-150s:1348 frames,
mean44.502ms, p9550.691, p9958.397 (22.47FPS), GPUmean13.635ms. Same2560x1440/RR1485x835,
modelF,VanillaPT,four bounces,Reflex1,FGoff,vsyncoff; shaders/options frozen with capture.
Renderer still prepares the static scene/TLAS and runs PT/RR/post/present; ticks, GUI, streaming
and dynamic geometry production are absent. Observed3.33x FPS is not a game optimization.
GPU timestamps also do not measure all CPU/native allocation/driver work.

Game reached8207 ready chunks by30.68s after entry and stayed at that count; sampled queues
were empty before capture, with small legitimate updates between samples. Save/native close
and exit0 were confirmed. Screenshot RGBMAE1.222/255; agent inspection agrees on camera/terrain;
independent temporal warmup and lack of user visual inspection prevent an equivalence claim.
Whole-card peaks13662/13454MiB include unrelated applications. Native VMA totals remain about
5129MiB; game steady totals5818-5836MiB. VMA's vram_mb label is not device-local-only VRAM.

Non-portable evidence: `D:\Workspaces\Artifacts\MCVRSceneReplay\20260924\iteration-7`,
`replay-medium-1` and `replay-medium-focused`. Native core
`E06B2AD5A6C76528EC5E59DFF4622E6F45D5AA7BDEED549A057C64C022DEA29F`/matching PDB reuse the
prior tested source manifest `280EB8F902EA4A615E55213A3F597DDA30BF064957D66DD1AD257DA135DEB113`;
loaded-module records, snapshot/runtime hashes, per-frame CSV and final images are retained.
Capture uses distinct DLL `CDE37551C50E95F654FC79C966F2DCA54038696254D20274DEB281BD5B029F4A`
from the unchanged Minecraft JAR. Only the external diagnostic was rebuilt in this step;
previous CTest/build results remain historical rather than being re-reported as new execution.
No renderer optimization, staging/commit/push, Prism changes or public binary distribution.
GPU-fault, long-term stability, moving-scene attribution and license boundaries remain open.

## 2026-09-24: Native consumers in the three-path CPU investigation

Status: investigating; optimization proposed. Evidence: static and existing JFR/allocation records.
Applies to: MCVR `4778983778d53084132ce84ed0e567579ed6ed7b` plus retained replay work;
Radiance `b8568cafd222c8169cd6c8fd3d5771690e3425ca`. No native source change/build in this step.

See the [paired investigation](../../Radiance/docs/research/2026-09-24-medium-scene-cpu-hotspots.md).
`Textures::queueUpload`/`ImageBufferCache::append` copy the complete source-image allocation into
host staging even when the GPU transfer selects one animation frame. Entity batch construction
allocates three aggregate geometry buffers plus AS storage/scratch each frame, so the earlier
114.6 allocations/s is not an entity or BLAS count. Java-cached clouds use the same uncached native
path. Neither record isolates CPU packing, allocation, transfer or completed GPU-AS cost.

The direct sampler check additionally found an asymmetric mipmap-mode comparison on regular
textures versus frame aliases. It is recorded as a concrete contract follow-up, with no claim
that it caused the measured slowdown or that it has been repaired. Existing generation checks,
descriptor ownership, in-flight retention and Flywheel/UI cache contracts must survive any later
optimization; global idle or freezing dynamic geometry are not acceptable substitutes.

Non-portable evidence: `D:\Workspaces\Artifacts\MCVRSceneReplay\20260924\hotspot-investigation`;
inspected-source manifest SHA-256
`48DF929BAFE1C84ED851EAAFF541538920F2F73C10F9BF3049AFB39958161A06`.
The prior JFR was re-exported with deeper stacks, not rerun. No new tests, game/GPU process,
deployment, product changes or Git staging/commit/push. Per-change gains and regression acceptance
remain unmeasured; previous runtime/build results retain their original scope.

## 2026-09-24: Compact texture staging and submitted cloud geometry reuse

Status: implemented; build-verified; automated-verified; runtime-observed; deployed with Radiance.
Evidence: source, behavior tests, GPU suite, bounded game comparison and reload regression.
Applies to MCVR `4778983778d53084132ce84ed0e567579ed6ed7b` plus retained replay work and this
uncommitted optimization; paired Radiance `b8568cafd222c8169cd6c8fd3d5771690e3425ca` worktree.
Supersedes the prior investigation's no-implementation boundary for this stage only.

`TextureUploadRegion` validates source stride/offset/extent; `Textures::queueUpload` also validates
the destination mip and stages only packed rows. `ImageBufferCache::appendRegion` aligns to both
Vulkan's existing four-byte rule and texel size, preserves earlier queued data during growth,
and accounts actual staged bytes. Existing staging detachment/retirement and descriptor/texture
ownership remain active. The Java side supplies only needed auxiliary rectangles and preserves
generation identity through the existing upload path.

`Vertex::appendPackedVertices` removes two intermediate vectors per geometry using the existing
field conversion functions. Explicit cloud capture separates its aggregate geometry storage
from ordinary entities. `SubmittedGeometryCache` admits reuse only after successful frame or
readback-flush queue submission; recorded-but-unsubmitted work is never reusable. New frames clone
entity transform metadata while sharing immutable geometry/BLAS ownership. Revision changes and
inactivity evict the bounded cache, while existing frame/history owners retain in-flight resources.
BLAS input/upload ordering uses the existing world transfer and AS barriers. No global idle,
camera culling or generic animated-entity caching was introduced. Ordinary batches still allocate.

RelWithDebInfo INSTALL build and CTest 56/56 passed. Added native behavior tests cover packed
rectangle content/guard bytes/overflow, exact vertex field equivalence, submission admission,
abandoned work, revision replacement and in-flight retention. These tests plus the paired Java
suite and actual cloud/reload game path complement each other; helper tests alone are not a GPU
ownership proof. The actual two JNI cloud entry points were exercised by the client.

The [paired ledger](../../Radiance/docs/DEVELOPMENT_LEDGER.md#2026-09-24-region-uploads-block-entity-index-and-submitted-cloud-cache)
owns configuration and full timing results: one focused Medium pair, same initial world/settings,
90-150 s real-frame mean 44.718 -> 41.180 ms (22.362 -> 24.284 FPS), GPU 14.261 -> 13.433 ms.
Entity allocation remains 258.97 -> 273.78 MB/s with higher frame throughput; normalized per-frame
bytes fell only about 2.7%. VMA totals include host allocations. This does not attribute individual
gains or prove general/long-term performance. Both roughly 162 s game runs saved and closed.
Separate 110.18 s reload/chest/cloud regression recorded 7 cloud build requests / 2,404 reuse
requests, returned to 8,207 ready sections and saved/closed normally. No device loss occurred in
these three processes; prior GPU-fault facts are not superseded.

Non-portable evidence: `D:\Workspaces\Artifacts\MCVRSceneReplay\20260924\optimization`;
product-source manifest SHA `C7EDE053BB8DE2597A6459D2D70820B9991B4E90B9721C6FB49F9A8DFBE29DA4`.
Core DLL `D296C930B0E65E71646695B6B7E42019570F0779DAB65310623D75571434CCA2` matches build,
JAR embedding, extraction and actual loaded-module records; matching PDB is retained.
Paired JAR `5508403622385BD98B07B76A42C2FD2AC64A3B19D61818D8DED7ADD96C21E605` and the diagnostic
mod were copied only to the authorized Prism mods directory. Visual/PBR/world-switch acceptance,
per-change attribution, dynamic-entity buffer pooling, the separate sampler follow-up and vendor
binary-distribution permission remain open. No staging, commit, amend, push or public release.

## 2026-09-24: Optional native audit provider boundary

Status: implemented; build-verified; automated-verified; bounded runtime-observed.
Evidence: C ABI tests, RelWithDebInfo INSTALL, CTest 57/57, matched DLL attachment and normal close.
Applies to MCVR `4778983778d53084132ce84ed0e567579ed6ed7b` plus preserved replay/hotspot work.

`diagnostics/audit_api.h` defines the versioned install-once provider; `alloc_trace.hpp` and
`fg_timing.hpp` are now thin event hooks. Allocation aggregation, timing summaries and formatting
live in Radiance's tracked `Modules/RadianceAudit/native/collector`, separately built and bundled
only with the diagnostic JAR. Without a provider, timing avoids the diagnostic clock/lock and
allocation events do not create maps/strings. Framework no longer writes periodic radiance-perf.log.
The provider cannot unload while native resources may report retirements. First-party collector
state is bounded and process-lived, with no STL ownership or exception crossing the C ABI.

Lifecycle injection requires an explicitly armed capability; ordinary error propagation and real
fatal/close logic remain unchanged. GPU readback request polling is gated. Native early-fault hooks,
GPU-owning capture/replay internals and other explicit traces are retained, not silently removed.
The built core DLL is `31A3357C8E690A29738BB5F38F6BEB6439E96025BA0DBC6B3FCB47B6A277A6B6`;
its install/JAR/extraction/actual-load identities agree. Full source, artifact and run evidence is in
[the paired ledger](../../Radiance/docs/DEVELOPMENT_LEDGER.md#2026-09-24-tracked-optional-radiance-audit-module-and-native-collector).
The preliminary missing-JNI-header build failed during concurrent header regeneration; sequential
rebuild succeeded. New checks do not repeat G1/G2, establish GPU-loss cause, prove world/visual
stability, or authorize vendor binary redistribution. No staging, commit, amend or push.

## 2026-09-24: One-shot scene replay retirement

Status: implemented; build-verified; automated-verified; deployed (no new gameplay acceptance).
Evidence: static, build, automated, GPU fixtures and deployment; applies to the dirty paired worktree.
Supersedes: active maintenance of the preceding standalone replay experiment, not its historical
measurements or captured evidence.

At the user's request remove `scene_replay.*`, the camera/relocation contract, standalone executable
and contract test; remove its world-prepare and output hooks, pipeline JSON description, buffer-address
registry and BLAS capture geometry fields. Source files and obsolete generated tool outputs are sent
to the Windows Recycle Bin. This restores the six replay-only integration files to their HEAD bytes;
world preparation retains the unrelated cached-cloud submission. Optimized uploads, packing, clouds,
resource retirement, failure handling and the optional Audit C ABI are preserved.

The general GPU readback/fault tooling is not this retired scene snapshot facility. Historical
captures/source archives under `D:\Workspaces\Artifacts\MCVRSceneReplay\20260924` (non-portable)
and Radiance's ignored run evidence remain intact. No dummy export, disabled implementation or
alternate active replay build target is retained. Java removes its one-shot scene controller too.

Verification and artifact evidence are recorded under the sibling Radiance
`run/replay-retirement-20260924`; the [paired retirement record](../../Radiance/docs/DEVELOPMENT_LEDGER.md#2026-09-24-one-shot-scene-replay-retirement-and-amend-preparation)
owns the shared build settings and source-hygiene correction. No new world or visual acceptance,
GPU root-cause closure, Git write or binary redistribution approval follows from this retirement.

### Executed validation

RelWithDebInfo INSTALL and shader/runtime installation passed. CTest passed 56/56 including GPU
fixtures; the only count reduction from 57 is deletion of the replay-only test. DLL export inspection
confirmed no replay entry and a retained Audit provider. The paired Java/package gates passed
(GAME 192, Audit 6, bootstrap 10 passed/2 skipped; native collector 1). No new client was started.

Final core SHA-256: `ABDA7889AF78F8C6F7FF7B3B897D53619580C1D1FABFE395C36D4246BBD17FB6`; build/install/JAR bytes match.
The paired product/build/test snapshot SHA-256 is `9180BCD9DC6B599A54678A2FB7FFA1030118672ABEF3AF1A1D985FBE7A040CD5`.
Full paths, Java artifact hashes and deployment identity are in the paired ledger/evidence manifest.
Root/meta/history are unchanged and the worktree remains unstaged; no amend, push or release occurred.

## 2026-09-24: User feedback and native source checkpoint

Status: build/automated evidence retained; bounded client observation and user feedback received.
The user reports no issue in actual play with the replay-retirement pair and authorizes source
amend/synchronization. Core DLL remains
`ABDA7889AF78F8C6F7FF7B3B897D53619580C1D1FABFE395C36D4246BBD17FB6`; native source matches the
preceding exact-byte snapshot. No native product or test changes are added in this checkpoint.

The [paired manual record](../../Radiance/docs/DEVELOPMENT_LEDGER.md#2026-09-24-manual-feedback-and-source-checkpoint-after-replay-retirement)
owns the two-world save/exit logs, installed artifact identity and residual Veil/map-data messages.
There is no fresh per-process DLL hash/native-close assertion, exhaustive rendering acceptance or
proof of long-term stability. Existing GPU/root-cause and binary-license boundaries remain open.

## 2026-09-24: Glow lichen opposite-face UV evidence

Status: specific model-input mechanism confirmed; no native product change or repair.
The paired [lichen investigation](../../Radiance/docs/DEVELOPMENT_LEDGER.md#2026-09-24-glow-lichen-opposite-face-uv-investigation)
owns the source-model, actual baked/PBR vertex and GPU evidence. On core
`ABDA7889AF78F8C6F7FF7B3B897D53619580C1D1FABFE395C36D4246BBD17FB6` (source
`b70d2a149fd86f8a03c81dc7b9c8bdcc5f7fae57`), eight primary floor readbacks retain all 105 opaque
texels, with zero mismatches among 54,880 interior samples. The source baker instead assigns
opposite horizontal faces UVs differing by a half-turn; only 48 opaque positions overlap.
Native vertex packing and the inspected Vanilla PT/Advanced hit consumers preserve and use the
selected face's UV, explaining how camera-facing and support-side light paths can disagree.

The new runtime case exercises Vanilla PT / RR D only; no new Advanced or Sable runtime claim.
No native shader, culling, BLAS, texture or emission setting was changed. A scoped correction and
its indirect-light/reflection acceptance remain pending; global two-sided-material normalization
would be unsafe for models intentionally using different surfaces. Existing lifecycle, device-loss
and redistribution boundaries are unchanged. See the paired record for interrupted runs and exact
artifact identities; no historical test results are reclassified as new tests.

## 2026-09-24: Glow lichen accepted without native changes

Status: reported symptom closed by explicit user acceptance; no product correction.
The [paired decision](../../Radiance/docs/DEVELOPMENT_LEDGER.md#2026-09-24-glow-lichen-accepted-as-normal-upstream-behavior)
supersedes the preceding pending UV-correction proposal. The user accepts the inherited model's
two opposing single-sided quads and their horizontal-face UV difference as normal behavior.
Retain existing native geometry, culling, UV and emission handling; no new build or client run
was performed for this documentation-only decision. Existing measurements are retained with
their original scope, without claiming physical equivalence or general visual acceptance.

## 2026-09-24: Diagram raster resource and composition re-audit

Status: investigating; static findings only, no native product change or new execution.
Applies to MCVR `ead8d47d80ad2bc9cf81740c29f0ec230b61f92f`, paired with Radiance
`6e9b97a53049fad833e673da647ac517efde5fe3` (clean before documentation changes).
The [paired re-audit](../../Radiance/docs/research/RASTER_PARITY_AND_PONDER_RETIREMENT.md#2026-09-24-diagram-semantic-re-audit-and-correction)
corrects the earlier incomplete diagram parity assessment; historical GPU fade tests remain
bounded evidence, not full original-renderer equivalence.

Native source inspection confirms that queued writes target one lightmap image before overlay
draw submission, so binding/lifetime snapshots do not retain the Java producer's different
block/BE/restored image contents. `beginDiagram` also shrinks the viewport to a clipped window
rectangle instead of preserving projection and clipping coverage alone. The native post embeds
default palettes/dither, unlike original resource-pack-resolved inputs. The report separates
these static gaps from unresolved Sable lighting/backend and visual questions. No device-loss
cause or new synchronization-validation result is inferred from this investigation.

Source/decompilation hashes and coverage are in sibling Radiance's ignored
`build/research/20260924-diagram-reaudit/EVIDENCE.json`. No build, CTest, GPU/client run or
deployment occurred. Future correction needs production-path stage-content/overlap readback,
fractional/offscreen composition, pack override and Java/native cleanup cases; tests of helper
names or default shader constants alone do not establish them. Existing Ponder archive, world
PT contracts, layout keepalive and GPU/licensing boundaries remain unchanged. No Git staging,
commit/amend or push.

## 2026-09-24: Diagram producer restoration uses existing Vulkan raster translation

Status: paired implementation build-verified; bounded GL/Vulkan runtime comparison; native product
unchanged. Applies to MCVR `ead8d47d80ad2bc9cf81740c29f0ec230b61f92f` with Radiance's uncommitted
diagram correction above `6e9b97a53049fad833e673da647ac517efde5fe3`.

The [paired implementation and evidence](../../Radiance/docs/DEVELOPMENT_LEDGER.md#2026-09-24-original-diagram-producer-restored-with-physical-resolution-presentation)
replaces the custom diagram caller with original Simulated group/post/GUI execution through existing
Vulkan raster/Veil translation. Separate immutable lightmaps preserve stage contents, original
layer/compile-time sort/AO semantics are restored, and physical-size targets retain original GUI
placement/fade. The previous native hardcoded diagram post/clipped viewport path is no longer used
by this UI; it was not deleted or modified in this batch. Ponder PT remains archived.

No native rebuild or CTest rerun was needed or claimed. DLL SHA-256 remains
`ABDA7889AF78F8C6F7FF7B3B897D53619580C1D1FABFE395C36D4246BBD17FB6`, checked in the package and actual
isolated client load. Final Vulkan PID 75476 completed rotation, GUI resize, reload, save and exit.
Its fixture material RGBA matches GL exactly; final paper alpha matches, with 19/81 remaining RGB
edge differences. Small extra regions in earlier runs remain unattributed; a later clean capture
does not explain them. This is bounded execution evidence, not all-raster visual acceptance.

Evidence resides in sibling Radiance `run/diagram-semantics-20260924/evidence/`, including normalized
native source file-list hash `cb4364f6fb213f2fa1efb59e287272ebe208fbcf0866c91af158aa07171b3065`.
Native product unchanged; only this ledger is modified. Mechanism/note/override/backend acceptance,
GPU-fault uncertainty and redistribution conditions remain open. No staging, commit/amend or push.

## 2026-09-25 Screen-effect audit and unresolved Veil underwater output

Superseded symptom report: see the [user retest correction below](#2026-09-25-veil-underwater-retest-correction-and-deferred-work).

Status: investigating. Evidence: source inspection and user-reported visual failure, not a new
native/GPU test or reproduced failure. Applies to the dirty worktree above
`ead8d47d80ad2bc9cf81740c29f0ec230b61f92f`; no native source or artifact changes in this step.

The [paired Radiance record](../../Radiance/docs/DEVELOPMENT_LEDGER.md#2026-09-25-screen-effect-static-audit-and-missed-veil-underwater-overlay)
owns the findings and correction: the user sees Veil's underwater texture missing, which the static
audit missed despite finding the Java/native capture path. Successful recording does not prove the
same frame's texture, descriptors, uniforms and HDR/final composition produce a visible overlay.
Root cause, process identity and affected configurations remain unconfirmed. Existing bounded
persistent-model visual feedback does not close this defect.

The same audit found unused PT lightmap-effect strengths and incomplete air/status-effect fog
selection in both PT shaders; declared fields and preserved Java producers are not end-to-end
support. Veil first-person post cancellation and generic shader-adapter restrictions are separate
compatibility boundaries, not established causes of the underwater symptom. Preserve historical
observations and use final-frame evidence for future closure. No tests, build, deployment, staging,
commit, amend or push were performed while recording this correction.

## 2026-09-25 Veil underwater retest correction and deferred work

Status: superseded for the missing-texture report; deferred for future visual work. Evidence:
user visual retest and bounded source inspection; no new native/GPU run or product modification.
The dirty native worktree above `ead8d47d80ad2bc9cf81740c29f0ec230b61f92f` is preserved.

The user confirms the underwater texture exists and is merely very faint in some scenes. No
disappearance defect or native fix was established; do not increase opacity from this report or
claim measured alpha/HDR parity. The
[paired correction](../../Radiance/docs/DEVELOPMENT_LEDGER.md#2026-09-25-veil-underwater-report-retracted-and-two-deferred-visual-tasks)
retains the earlier mistaken report, directed inspection and remaining evidence boundary.

The user also requests deferred labPBR grazing-angle black correction with Sundial as a reference,
and a vanilla-cloud rewrite. Radiance's existing roadmap/research own these cross-repository items;
neither is implemented here and the cloud work is not limited to the previous fog proposal.
No native source, shader, test, artifact or deployment changed; no build, staging, commit, amend
or push accompanied this documentation update.

## 2026-09-25: Split the non-performance source checkpoint

Status: source separation verified; paired build/automated gates passed within the limits below.
Evidence: candidate trees, complete file hashes, isolated Java/Audit build and test output.
Applies to the split above native `ead8d47d80ad2bc9cf81740c29f0ec230b61f92f` and Radiance
`6e9b97a53049fad833e673da647ac517efde5fe3`; historical run dates/artifacts are unchanged.

The user authorizes amending only the diagram/visual investigation records into native
`Initial port`. All 61 modified/new native source, shader, test and diagnostic files, together
with their performance ledger entries, remain uncommitted for the separately authorized later
`Performance Optimization Test V1` checkpoint. No native product, dependency or build input is
changed in this amendment. The original author/committer identity, both dates/time zones,
message and upstream parent are retained, with a fresh SSH signature.

The isolated paired source is checked against the archived diagram handoff. Its Java gates
report GAME 196 passed / 1 GPU-harness case skipped, bootstrap 10 passed / 2 GPU cases skipped,
Audit 9 passed, and the separately built original Audit native collector 1/1 passed.
Distribution contents and the distinct Maven development artifact are verified; the public binary
gate rejects redistribution as expected. Native MCVR is not rebuilt or retested in this
source-unchanged step. Reused DLL identity is the original diagram `ABDA7889AF78F8C6F7FF7B3B897D53619580C1D1FABFE395C36D4246BBD17FB6`.

Non-portable operation evidence: `D:/Workspaces/Artifacts/RadianceCommitSplit/20260925/`, including
candidate diffs, raw working-file hashes, normalized initial-product manifests and test XML/logs.
The first isolated packaging check ran before runtime copying completed and reported a missing
DLSS DLL; after complete hash-verified extraction, package/Maven checks pass. No product bypass
was added. No client is run or deployment changed. Prior GPU loss, diagram visual limits and
vendor redistribution conditions remain open. The paired Radiance ledger owns the one-way
native commit reference; there is no reciprocal new Radiance SHA backfill.
