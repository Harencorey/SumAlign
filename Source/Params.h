#pragma once
// Automatable parameter IDs. Don't rename these after release: it breaks saved sessions.

namespace sa::pid
{
    inline constexpr const char* delay       = "delay";        // ms, -10 .. +10 (+ = later)
    inline constexpr const char* invert      = "invert";       // polarity
    inline constexpr const char* phase       = "phase";        // degrees, 0 .. 180
    inline constexpr const char* phaseCenter = "phaseCenter";  // Hz
}

namespace sa
{
    inline constexpr float kPhaseCenterMin = 20.0f, kPhaseCenterMax = 8000.0f, kPhaseCenterDefault = 250.0f;
    inline constexpr double kSpeedOfSound = 343.0;   // m/s at ~20 C, for the distance readout
}
