// Canonical pattern definition list.
// This is the *only* place a new PatternPreset should be added.
//
// X(preset_id, token_name, display_name, caps_bitmask, algo_id, dynamic_base)
//
// Notes:
// - preset_id and algo_id are intentionally decoupled to allow multiple presets to share one algo if desired.
// - token_name is the canonical CLI/config token.
// - caps_bitmask is a PatternPresetCaps mask describing which params are meaningful for this preset.
// dynamic_base:
//   - false: base pixels depend only on PatternSpec (cacheable).
//   - true:  base pixels also depend on per-frame overlay inputs (dynamic; base cache is bypassed).
X(XyXor,          "xy_xor",         "XY XOR",          kCapsSeed,                     XyXor,          false)
X(Solid,          "solid",          "Solid",           kCapsSeed | kCapsRgba,         Solid,          false)
X(Checker,        "checker",        "Checkerboard",    kCapsSeed | kCapsCheckerSize,  Checker,        false)
X(ColorBars,      "color_bars",     "Color Bars",      kCapsNone,                     ColorBars,      false)
X(RadialGradient, "radial",         "Radial Gradient", kCapsNone,                     RadialGradient, false)
X(CornersRgba,    "corners_rgba",   "Corners RGBA",    kCapsSeed | kCapsRgba,         CornersRgba,    false)
X(Noise,          "noise",          "Noise (Static)",  kCapsSeed,                     Noise,          false)
