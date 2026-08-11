# MCVR development records

MCVR is maintained with the sibling Radiance repository. Cross-repository product decisions,
deferred work, and the documentation rules are canonical in Radiance:

- [Documentation policy](https://github.com/RecRivenVI/Radiance/blob/develop/docs/DOCUMENTATION_POLICY.md)
- [Radiance development ledger](https://github.com/RecRivenVI/Radiance/blob/develop/docs/DEVELOPMENT_LEDGER.md)
- [Radiance roadmap](https://github.com/RecRivenVI/Radiance/blob/develop/docs/ROADMAP.md)

This repository keeps native evidence discoverable on its own:

- [`DEVELOPMENT_LEDGER.md`](DEVELOPMENT_LEDGER.md) records implemented C++/shader/build changes,
  native validation, artifact identity, and corrections.
- [`research/NVIDIA_FRAME_GENERATION_AND_NEURAL_RENDERING.md`](research/NVIDIA_FRAME_GENERATION_AND_NEURAL_RENDERING.md)
  records MFG, unofficial neural rendering, and GUI/background-blur FG research.
- [`research/NATIVE_RENDERER_INVESTIGATIONS.md`](research/NATIVE_RENDERER_INVESTIGATIONS.md) records
  RenderPearl host migration, Ponder resource pressure, NRD contracts, and PT material backlogs.
- [Radiance native crash and runtime history](https://github.com/RecRivenVI/Radiance/blob/develop/docs/research/CRASH_AND_RUNTIME_INVESTIGATION_HISTORY.md)
  records the cross-repository access-violation, descriptor-lifetime, device-loss, and driver-state
  evidence.

Local sibling checkout for the canonical policy: `../Radiance/docs/DOCUMENTATION_POLICY.md`. This
path is an environment convention, not a portable repository link.
