# Native renderer investigation register

Observed on: 2026-09-21.
Status: proposed; static investigation only.
Sources: MCVR worktree later consolidated as
`5150670796380bf128fecec551c864f480bb05f9`. Revalidate against later commits before implementation.

This register summarizes MCVR-owned findings from Codex tasks
`01a0bcc0-15e4-7f50-809c-4dc0062f2e20` and
`01a0bcc7-1289-7201-9770-c4e3ec574347`, read on 2026-09-21. Detailed game-side requests are kept in
the sibling Radiance repository under `docs/research/`. Nothing in this file is marked implemented.

## RenderPearl host migration

If RenderPearl is backported, MCVR must stop being a second Vulkan host. The extended RenderPearl
backend becomes the single owner of instance, physical/logical device, allocator, queues, surface,
swapchain, submission timeline, and present. MCVR borrows those resources and exposes PT,
reconstruction, and Streamline stages inside the host frame graph.

The host's device requirement negotiation must happen before creation and include ray-tracing
pipeline, acceleration structures, buffer device address, descriptor indexing, NGX/Streamline,
XeSS, and any selected feature extensions. Streamline initialization/requirements cannot be bolted
on after RenderPearl has already created the device and swapchain.

The minimum native proof is one host device, one swapchain owner, one MCVR compute/RT stage writing a
host texture, explicit synchronization, resize/reload/shutdown coverage, and clean Vulkan validation.
This work is subordinate to the isolated vertical slice in Radiance
`docs/research/RENDERPEARL_SODIUM_MIGRATION.md`.

## Ponder resource pressure

The preliminary investigation found full-window allocations, per-frame descriptor/output creation,
repeated geometry/AS work, independent world pipeline state per scene, and two live pipelines during
transitions. Native work should reuse frame-context images/descriptors, accept a Ponder viewport or
internal resolution, retain static buffers/BLAS by content fingerprint, refit changed transforms,
and support a frozen outgoing-scene snapshot. Add separate timing/allocation counters for upload,
BLAS, TLAS, trace, denoise/upscale, and transition overlap.

## NRD signal contract

The inspected MCVR snapshot merges direct radiance into the diffuse signal sent to REBLUR,
supplies primary camera depth as diffuse hit distance, and uses a 100-pixel maximum blur with a
large prepass and long history. It then applies an additional temporal accumulation path in the
NRD preset. The proposed correction is to produce the lobe hit distance and preserve direct
visibility before tuning parameters. Compare the pinned baseline, contract-corrected defaults, and
each later tuning step independently.

## PT material and visibility backlog

- Carry one-sided material semantics through chunks, entities, `WorldMeshSink`, Flywheel, camera,
  reflection, and shadow rays. Opaque flags must not suppress required software any-hit culling.
- Replace noisy ordinary alpha blending with deterministic post-denoise layer/OIT composition while
  retaining genuine refractive/absorptive media in PT.
- Add explicit medium identity, sane IOR defaults/overrides, nested-medium tracking, and
  distance-based absorption.
- Correct normal decode, tangent handedness, grazing constraints, shadow terminators, and indirect
  sample rejection before attempting higher-quality microfacet normal mapping.
- Replace hard-coded celestial VMF concentration with a bounded angular-radius parameter and apply
  one consistent transformed celestial direction to lighting and sky evaluation.
- Add a cloud-only fog calculation before the existing cloud `skipFog` boundary.

Each item requires deterministic shader fixtures or diagnostic views before product tuning. A shader
compile or visually plausible frame is not proof of correct camera, reflection, shadow, and history
semantics.
