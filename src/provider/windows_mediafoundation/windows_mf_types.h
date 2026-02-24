// src/provider/windows_mediafoundation/windows_mf_types.h
#pragma once

#ifdef _WIN32

#include <cstdint>

#include "provider/provider_contract_datatypes.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>

namespace cambang {

inline ProviderError provider_error_from_hr(HRESULT hr) {
  if (hr == S_OK) return ProviderError::OK;
  if (hr == MF_E_SHUTDOWN) return ProviderError::ERR_SHUTTING_DOWN;
  if (hr == E_INVALIDARG) return ProviderError::ERR_INVALID_ARGUMENT;
  if (hr == MF_E_INVALIDREQUEST) return ProviderError::ERR_BAD_STATE;
  if (hr == MF_E_NOT_FOUND) return ProviderError::ERR_INVALID_ARGUMENT;
  if (hr == E_OUTOFMEMORY) return ProviderError::ERR_PROVIDER_FAILED;
  return ProviderError::ERR_PROVIDER_FAILED;
}

inline uint32_t fourcc_from_mf_subtype(const GUID& subtype) {
  // IMPORTANT (byte order):
  // The common "RGB32"/"ARGB32" uncompressed subtypes are laid out in memory as:
  //   Byte0=Blue, Byte1=Green, Byte2=Red, Byte3=Alpha or don't-care
  // (i.e. BGRA in memory on little-endian CPUs).
  if (subtype == MFVideoFormat_RGB32 || subtype == MFVideoFormat_ARGB32) {
    return FOURCC_BGRA;
  }
  // NOTE: Some SDKs do not define a dedicated "RGBA32" subtype GUID.
  // For this minimal dev provider we only accept the widely-available RGB32/ARGB32
  // family (which is BGRA in memory on little-endian).
  return 0;
}

} // namespace cambang

#endif // _WIN32
