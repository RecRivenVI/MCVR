#ifndef UI_PRIMARY_VISIBILITY_GLSL
#define UI_PRIMARY_VISIBILITY_GLSL
const uint rayUiPrimaryBit = 1u << 20u;
uint uiPrimaryTraceState(uint stateBits, uint step) {
    // Primary-surface replacement also traces reflected/refracted continuations.
    // Those rays must see both transition worlds, just like indirect rays.
    return (stateBits & ~rayUiPrimaryBit) | (step == 0u ? rayUiPrimaryBit : 0u);
}
bool acceptsUiOwner(uint materialFlags, uint requestedOwner, uint stateBits) {
    uint owner = materialFlags >> 24u;
    return requestedOwner == 0u || owner == 0u || owner == requestedOwner ||
        (stateBits & rayUiPrimaryBit) == 0u;
}
#endif
