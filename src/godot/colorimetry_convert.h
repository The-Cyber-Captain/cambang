#pragma once

// Colorimetry -> Dictionary, shared by every result surface that reports it.
// One definition on purpose: the dictionary shape is public API, and two
// copies would be free to drift apart.

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

inline const char* color_range_name(ColorRange value) {
  switch (value) {
    case ColorRange::LIMITED: return "limited";
    case ColorRange::FULL: return "full";
    case ColorRange::UNSPECIFIED: return "unspecified";
  }
  return "unspecified";
}

inline const char* color_matrix_name(ColorMatrix value) {
  switch (value) {
    case ColorMatrix::BT601: return "bt601";
    case ColorMatrix::BT709: return "bt709";
    case ColorMatrix::BT2020_NCL: return "bt2020_ncl";
    case ColorMatrix::UNSPECIFIED: return "unspecified";
  }
  return "unspecified";
}

inline const char* color_transfer_name(ColorTransfer value) {
  switch (value) {
    case ColorTransfer::BT709: return "bt709";
    case ColorTransfer::SRGB: return "srgb";
    case ColorTransfer::PQ: return "pq";
    case ColorTransfer::HLG: return "hlg";
    case ColorTransfer::UNSPECIFIED: return "unspecified";
  }
  return "unspecified";
}

inline const char* color_primaries_name(ColorPrimaries value) {
  switch (value) {
    case ColorPrimaries::BT709: return "bt709";
    case ColorPrimaries::BT2020: return "bt2020";
    case ColorPrimaries::P3_D65: return "p3_d65";
    case ColorPrimaries::UNSPECIFIED: return "unspecified";
  }
  return "unspecified";
}

// Colour interpretation of a member's retained payload, reported verbatim.
//
// "unspecified" is truthful absence, not a value -- the same stance the
// provider contract takes on PayloadColorimetry. Nothing here substitutes a
// default, because a caller writing its own Y'CbCr maths needs to know whether
// CamBANG was told the colour space or is simply unaware of it. What CamBANG's
// own CPU conversion assumes when told nothing is documented in
// pixel_payload_and_result_contract.md 11.6.1; it is deliberately not reported
// here as though the provider had declared it.
inline godot::Dictionary colorimetry_to_dict(const PayloadColorimetry& colorimetry) {
  godot::Dictionary out;
  out["range"] = godot::String(color_range_name(colorimetry.range));
  out["matrix"] = godot::String(color_matrix_name(colorimetry.matrix));
  out["transfer"] = godot::String(color_transfer_name(colorimetry.transfer));
  out["primaries"] = godot::String(color_primaries_name(colorimetry.primaries));
  out["declared"] = colorimetry.range != ColorRange::UNSPECIFIED ||
                    colorimetry.matrix != ColorMatrix::UNSPECIFIED ||
                    colorimetry.transfer != ColorTransfer::UNSPECIFIED ||
                    colorimetry.primaries != ColorPrimaries::UNSPECIFIED;
  return out;
}

} // namespace cambang
