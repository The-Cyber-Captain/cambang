#include "imaging/synthetic/scenario_loader_validate.h"

#include <unordered_set>

#include "pixels/pattern/pattern_registry.h"

namespace cambang {
namespace {

void set_error(std::string* error, const std::string& message) {
  if (error) {
    *error = message;
  }
}

bool valid_stream_intent_token(const std::string& token) {
  return token == "PREVIEW" || token == "VIEWFINDER";
}

bool valid_pattern_preset_token(const std::string& token) {
  return find_preset_info_by_name(token) != nullptr;
}

enum class LoaderActionType : std::uint8_t {
  OpenDevice = 0,
  CreateStream,
  StartStream,
  StopStream,
  DestroyStream,
  CloseDevice,
  UpdateStreamPicture,
  Invalid,
};

LoaderActionType parse_loader_action_type(const std::string& token) {
  if (token == "OpenDevice") return LoaderActionType::OpenDevice;
  if (token == "CreateStream") return LoaderActionType::CreateStream;
  if (token == "StartStream") return LoaderActionType::StartStream;
  if (token == "StopStream") return LoaderActionType::StopStream;
  if (token == "DestroyStream") return LoaderActionType::DestroyStream;
  if (token == "CloseDevice") return LoaderActionType::CloseDevice;
  if (token == "UpdateStreamPicture") return LoaderActionType::UpdateStreamPicture;
  return LoaderActionType::Invalid;
}

} // namespace

bool validate_parsed_synthetic_scenario_loader_document(
    const SyntheticScenarioLoaderParsedDocument& parsed,
    std::string* error) {
  if (parsed.schema_version != 1) {
    set_error(error, "schema_version must be exactly 1");
    return false;
  }

  std::unordered_set<std::string> device_keys;
  device_keys.reserve(parsed.devices.size());
  for (size_t i = 0; i < parsed.devices.size(); ++i) {
    const auto& d = parsed.devices[i];
    if (d.key.empty()) {
      set_error(error, "devices[" + std::to_string(i) + "].key must be non-empty");
      return false;
    }
    if (!device_keys.emplace(d.key).second) {
      set_error(error, "duplicate device key: " + d.key);
      return false;
    }
  }

  std::unordered_set<std::string> stream_keys;
  stream_keys.reserve(parsed.streams.size());
  for (size_t i = 0; i < parsed.streams.size(); ++i) {
    const auto& s = parsed.streams[i];
    if (s.key.empty()) {
      set_error(error, "streams[" + std::to_string(i) + "].key must be non-empty");
      return false;
    }
    if (!stream_keys.emplace(s.key).second) {
      set_error(error, "duplicate stream key: " + s.key);
      return false;
    }
    if (s.device_key.empty()) {
      set_error(error, "streams[" + std::to_string(i) + "].device_key must be non-empty");
      return false;
    }
    if (device_keys.find(s.device_key) == device_keys.end()) {
      set_error(error, "streams[" + std::to_string(i) + "].device_key references unknown device key: " + s.device_key);
      return false;
    }
    if (!valid_stream_intent_token(s.intent)) {
      set_error(error, "streams[" + std::to_string(i) + "].intent has invalid value: " + s.intent);
      return false;
    }
  }

  for (size_t i = 0; i < parsed.timeline.size(); ++i) {
    const auto& a = parsed.timeline[i];
    const std::string ctx = "timeline[" + std::to_string(i) + "]";

    const LoaderActionType action_type = parse_loader_action_type(a.type);
    if (action_type == LoaderActionType::Invalid) {
      if (a.type == "EmitFrame") {
        set_error(error, ctx + ".type EmitFrame is not supported by strict v1 loader");
      } else {
        set_error(error, ctx + ".type is unknown: " + a.type);
      }
      return false;
    }

    if (a.has_device_key && a.device_key.empty()) {
      set_error(error, ctx + ".device_key must be non-empty when provided");
      return false;
    }
    if (a.has_stream_key && a.stream_key.empty()) {
      set_error(error, ctx + ".stream_key must be non-empty when provided");
      return false;
    }

    switch (action_type) {
      case LoaderActionType::OpenDevice:
      case LoaderActionType::CloseDevice:
        if (!a.has_device_key || a.has_stream_key || a.has_picture) {
          set_error(error, ctx + " must contain only device_key");
          return false;
        }
        if (device_keys.find(a.device_key) == device_keys.end()) {
          set_error(error, ctx + ".device_key references unknown device key: " + a.device_key);
          return false;
        }
        break;

      case LoaderActionType::CreateStream:
      case LoaderActionType::StartStream:
      case LoaderActionType::StopStream:
      case LoaderActionType::DestroyStream:
        if (!a.has_stream_key || a.has_device_key || a.has_picture) {
          set_error(error, ctx + " must contain only stream_key");
          return false;
        }
        if (stream_keys.find(a.stream_key) == stream_keys.end()) {
          set_error(error, ctx + ".stream_key references unknown stream key: " + a.stream_key);
          return false;
        }
        break;

      case LoaderActionType::UpdateStreamPicture:
        if (!a.has_stream_key || a.has_device_key || !a.has_picture) {
          set_error(error, ctx + " must contain stream_key and picture only");
          return false;
        }
        if (stream_keys.find(a.stream_key) == stream_keys.end()) {
          set_error(error, ctx + ".stream_key references unknown stream key: " + a.stream_key);
          return false;
        }
        if (!valid_pattern_preset_token(a.picture.preset)) {
          set_error(error, ctx + ".picture.preset has invalid token: " + a.picture.preset);
          return false;
        }
        break;

      case LoaderActionType::Invalid:
        break;
    }
  }

  return true;
}

} // namespace cambang
