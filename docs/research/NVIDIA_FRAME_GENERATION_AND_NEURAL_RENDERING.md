# NVIDIA frame generation and neural-rendering investigations

Observed on: 2026-09-21.
Status: proposed; static and external-source investigation only.
Sources: MCVR worktree later consolidated as
`5150670796380bf128fecec551c864f480bb05f9`; exact Streamline, DLSS-G, and community-runtime
versions are identified below. Revalidate before implementation or distribution.

This record preserves the requested behavior and preliminary findings from Codex task
`01a0bc58-51db-7831-a31c-d5afed4376c9`, read on 2026-09-21. The task examined third-party Ada
multi-frame generation, unofficial neural rendering, and GUI/background-blur interaction with
frame generation. No implementation or game-runtime acceptance was performed in that task.

## Requested direction

- Investigate fixed multi-frame generation on RTX 40/Ada instead of limiting MCVR to 2x FG.
- Investigate a Vulkan-native neural-rendering pass for RTX 4080 SUPER.
- Keep SR, RR, FG, Reflex, NRD, FSR, and XeSS independently selectable where their contracts permit.
- Ensure FG interpolates the world rather than GUI, and preserve correct menu/background-blur
  behavior.
- Treat display output and latency evidence as distinct from Streamline API success or present
  counters.

## MCVR baseline observed by the investigation

- Streamline 2.14.1 and DLSS-G 310.9.1 were integrated.
- MCVR supplies depth, motion vectors, HUD-less color, UI alpha, frame constants, and Reflex state.
- FG currently sets `numFramesToGenerate=1`, which requests one generated frame per rendered frame.
- Streamline initialization and feature loading are controlled by MCVR.
- UI is logically separated from the captured world, but the UI coverage resource is reconstructed
  from the RGB difference between final output and HUD-less output rather than from true UI color
  and alpha.

These facts were rechecked against the post-amend source on 2026-09-21. They are source facts, not
proof of displayed interpolation quality.

## Fixed Ada multi-frame generation

Preliminary feasibility is high for an experimental, exact-version backend:

1. Expose only `2x`, `Experimental 3x`, and `Experimental 4x` initially.
2. Require an Ada GPU and exact Streamline/DLSS-G versions, hashes, and unique byte signatures.
3. Patch only a validated provider/wrapper ceiling and capability gate in memory.
4. Include a validated temporal-position fix or kernel retarget so multiple generated frames do not
   all land at the same midpoint.
5. Set `numFramesToGenerate = multiplier - 1` from MCVR.
6. Fail closed to ordinary 2x when any signature, limit, capability, or runtime check differs.

Do not expose Dynamic MFG in the Vulkan product path while the official contract remains D3D12-only.
Do not infer working MFG from a larger reported maximum or from `numFramesActuallyPresented` alone.
Acceptance requires external display-change timing such as FrameView `MsBetweenDisplayChange`, plus
RTSS/PresentMon, frame-by-frame visual inspection, and latency behavior.

## Unofficial neural rendering

A community implementation demonstrated a Vulkan-facing `nvngx_dlssnr.dll` path using an
undocumented NGX feature, color/depth/motion inputs, reset/HDR/exposure/motion scaling, and a caller
gate. MCVR already produces the main inputs and can place a first experiment immediately before
DLSS Super Resolution.

Safe first prototype boundary:

- Windows, NVIDIA Vulkan, and the observed RTX 4080 SUPER test machine only;
- one Pre-SR pass at working scale 1.0;
- use an owned scratch image instead of overwriting Radiance HDR input in place;
- feed the result into DLSS SR;
- do not combine with RR, MFG, multipass, finished-picture, residual FG, or async-latest initially;
- require a user-supplied runtime with an exact allow-listed SHA-256;
- never bundle or download the unofficial runtime automatically;
- mark the path explicitly as unsupported/unofficial and fail cleanly when absent or mismatched.

The reference forwarder and Vulkan implementation investigated were GPL-3.0. Do not copy source
until MCVR's license and attribution policy have been decided. Technical feasibility does not settle
distribution, signature, maintenance, or security risk.

## GUI and background blur under FG

The world is captured before GUI draw, but background blur later rewrites the already-composited
full-screen target. Comparing final RGB against HUD-less RGB therefore marks nearly the entire screen
as UI whenever a full-screen blur is active. Streamline cannot reconstruct a new Minecraft blur for
each generated intermediate world frame from an alpha composite alone.

Required policy:

- **Ordinary HUD/GUI:** retain HUD-less world input and provide real UI color plus alpha/coverage.
  Do not infer alpha from a binary RGB difference.
- **Full-screen background blur:** disable FG for those frames, retain resources while off, mark the
  frame as not rendering game frames, and reset history when FG resumes.
- **Localized blur panel:** mark the blurred region as fully UI-covered; allow the world outside the
  panel to be generated.
- Detect blur from the actual native/UI post-blur request rather than a hard-coded screen-class list,
  so third-party blur callers follow the same rule.

`eEnableFullscreenMenuDetection` is not a replacement for this policy, especially while only
`UIAlpha` is supplied and true `UIColorAndAlpha` is absent.

## Implementation order and evidence gates

1. Replace difference-derived UI coverage with an explicit UI color/alpha contract and implement
   full-screen/local-blur FG policy.
2. Build the exact-version 3x MFG PoC and verify temporal spacing and display changes.
3. Open 4x only after 3x passes; keep higher multipliers hidden.
4. Build the isolated single-pass Pre-SR NR PoC with strict runtime provenance checks.
5. Test static scenes, motion, disocclusion, teleport/history reset, HDR, VRAM lifetime, and missing
   runtime fallback.
6. Test combinations only after each individual feature passes.

API return codes, hidden-window probes, accepted Vulkan presents, and in-game counters do not prove
what the monitor displayed. Record each evidence class separately.
