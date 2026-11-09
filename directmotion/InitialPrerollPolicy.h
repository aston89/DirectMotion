#pragma once

// Cold-start DirectShow preroll is not a user-requested paused-frame step.
// While the graph is transitioning Stopped -> Paused, source samples must be
// admitted to AviSynth/MVTools so its bounded bootstrap can reach the renderer.
// A strict Running -> Paused hold always takes precedence.
namespace DirectMotionInitialPrerollPolicy {

inline bool AllowColdStartPreroll(bool initialPrerollActive,
                                  bool runActive,
                                  bool pauseHoldActive) noexcept {
    return initialPrerollActive && !runActive && !pauseHoldActive;
}

} // namespace DirectMotionInitialPrerollPolicy