// Canonical pattern definition list.
// This is the *only* place a new PatternPreset should be added.
//
// X(preset_id, token_name, display_name, caps_bitmask, algo_id)
//
// Notes:
// - preset_id and algo_id are intentionally decoupled to allow multiple presets to share one algo if desired.
// - token_name is the canonical CLI/config token.
// - caps_bitmask is a PatternPresetCaps mask describing which params are meaningful for this preset.
X(XyXor,          "xy_xor",     "XY XOR",          kCapsSeed,                     XyXor)
X(Solid,          "solid",      "Solid",           kCapsSeed | kCapsRgba,          Solid)
X(Checker,        "checker",    "Checkerboard",    kCapsSeed | kCapsCheckerSize,   Checker)
X(ColorBars,      "color_bars", "Color Bars",      kCapsNone,                     ColorBars)
X(RadialGradient, "radial",     "Radial Gradient", kCapsNone,                     RadialGradient)
