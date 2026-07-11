#include "core/camera_concurrency_adc.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>

namespace cambang::camera_concurrency {
namespace {

void set_error(LoadResult& out,
               LoadErrorKind error_kind,
               const std::string& message) {
  out.ok = false;
  out.error_kind = error_kind;
  out.error_message = message;
  out.truth = {};
}

struct JsonValue {
  enum class Type : std::uint8_t {
    Null = 0,
    Bool,
    Number,
    String,
    Array,
    Object,
  };

  Type type = Type::Null;
  bool bool_value = false;
  std::int64_t number_value = 0;
  std::string string_value{};
  std::vector<JsonValue> array_value{};
  std::vector<std::pair<std::string, JsonValue>> object_value{};
};

class JsonParser final {
public:
  explicit JsonParser(const std::string& text) : text_(text) {}

  bool parse(JsonValue& out, std::string* error) {
    pos_ = 0;
    if (!parse_value(out, error)) {
      return false;
    }
    skip_ws();
    if (pos_ != text_.size()) {
      if (error) {
        *error = "unexpected trailing input at byte " + std::to_string(pos_);
      }
      return false;
    }
    return true;
  }

private:
  void skip_ws() {
    while (pos_ < text_.size() &&
           std::isspace(static_cast<unsigned char>(text_[pos_]))) {
      ++pos_;
    }
  }

  bool consume(char c) {
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool match_literal(const char* lit) {
    size_t i = 0;
    while (lit[i] != '\0') {
      if (pos_ + i >= text_.size() || text_[pos_ + i] != lit[i]) {
        return false;
      }
      ++i;
    }
    pos_ += i;
    return true;
  }

  bool parse_value(JsonValue& out, std::string* error) {
    skip_ws();
    if (pos_ >= text_.size()) {
      if (error) {
        *error = "unexpected end of json";
      }
      return false;
    }

    const char c = text_[pos_];
    if (c == '{') {
      return parse_object(out, error);
    }
    if (c == '[') {
      return parse_array(out, error);
    }
    if (c == '"') {
      out.type = JsonValue::Type::String;
      return parse_string(out.string_value, error);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      out.type = JsonValue::Type::Number;
      return parse_integer(out.number_value, error);
    }
    if (match_literal("true")) {
      out.type = JsonValue::Type::Bool;
      out.bool_value = true;
      return true;
    }
    if (match_literal("false")) {
      out.type = JsonValue::Type::Bool;
      out.bool_value = false;
      return true;
    }
    if (match_literal("null")) {
      out.type = JsonValue::Type::Null;
      return true;
    }

    if (error) {
      *error = "unexpected token at byte " + std::to_string(pos_);
    }
    return false;
  }

  bool parse_string(std::string& out, std::string* error) {
    if (!consume('"')) {
      if (error) {
        *error = "expected string at byte " + std::to_string(pos_);
      }
      return false;
    }

    std::string value;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') {
        out = std::move(value);
        return true;
      }
      if (c == '\\') {
        if (pos_ >= text_.size()) {
          if (error) {
            *error = "unterminated escape in string";
          }
          return false;
        }
        const char e = text_[pos_++];
        switch (e) {
          case '"': value.push_back('"'); break;
          case '\\': value.push_back('\\'); break;
          case '/': value.push_back('/'); break;
          case 'b': value.push_back('\b'); break;
          case 'f': value.push_back('\f'); break;
          case 'n': value.push_back('\n'); break;
          case 'r': value.push_back('\r'); break;
          case 't': value.push_back('\t'); break;
          case 'u': {
            if (pos_ + 4 > text_.size()) {
              if (error) {
                *error = "incomplete unicode escape in string";
              }
              return false;
            }
            unsigned int code = 0;
            for (int i = 0; i < 4; ++i) {
              const char h = text_[pos_++];
              code <<= 4;
              if (h >= '0' && h <= '9') {
                code |= static_cast<unsigned int>(h - '0');
              } else if (h >= 'a' && h <= 'f') {
                code |= static_cast<unsigned int>(h - 'a' + 10);
              } else if (h >= 'A' && h <= 'F') {
                code |= static_cast<unsigned int>(h - 'A' + 10);
              } else {
                if (error) {
                  *error = "invalid unicode escape in string";
                }
                return false;
              }
            }
            if (code > 0x7F) {
              if (error) {
                *error =
                    "non-ascii unicode escape is not supported in strict parser";
              }
              return false;
            }
            value.push_back(static_cast<char>(code));
            break;
          }
          default:
            if (error) {
              *error = "invalid escape sequence in string";
            }
            return false;
        }
      } else {
        if (static_cast<unsigned char>(c) < 0x20) {
          if (error) {
            *error = "control character in string";
          }
          return false;
        }
        value.push_back(c);
      }
    }

    if (error) {
      *error = "unterminated string";
    }
    return false;
  }

  bool parse_integer(std::int64_t& out, std::string* error) {
    const size_t start = pos_;
    bool neg = false;
    if (consume('-')) {
      neg = true;
    }
    if (pos_ >= text_.size() ||
        !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
      if (error) {
        *error = "invalid number at byte " + std::to_string(start);
      }
      return false;
    }
    if (text_[pos_] == '0' && pos_ + 1 < text_.size() &&
        std::isdigit(static_cast<unsigned char>(text_[pos_ + 1]))) {
      if (error) {
        *error = "leading zero is not allowed for numbers";
      }
      return false;
    }

    std::int64_t value = 0;
    while (pos_ < text_.size() &&
           std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
      const int digit = text_[pos_] - '0';
      if (value >
          (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
        if (error) {
          *error = "integer overflow in number literal";
        }
        return false;
      }
      value = value * 10 + digit;
      ++pos_;
    }

    if (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '.' || c == 'e' || c == 'E') {
        if (error) {
          *error = "non-integer numbers are not supported in strict parser";
        }
        return false;
      }
    }

    out = neg ? -value : value;
    return true;
  }

  bool parse_array(JsonValue& out, std::string* error) {
    if (!consume('[')) {
      if (error) {
        *error = "expected '[' at byte " + std::to_string(pos_);
      }
      return false;
    }

    out = {};
    out.type = JsonValue::Type::Array;

    skip_ws();
    if (consume(']')) {
      return true;
    }

    for (;;) {
      JsonValue element{};
      if (!parse_value(element, error)) {
        return false;
      }
      out.array_value.push_back(std::move(element));

      skip_ws();
      if (consume(']')) {
        return true;
      }
      if (!consume(',')) {
        if (error) {
          *error = "expected ',' or ']' in array at byte " +
                   std::to_string(pos_);
        }
        return false;
      }
    }
  }

  bool parse_object(JsonValue& out, std::string* error) {
    if (!consume('{')) {
      if (error) {
        *error = "expected '{' at byte " + std::to_string(pos_);
      }
      return false;
    }

    out = {};
    out.type = JsonValue::Type::Object;

    skip_ws();
    if (consume('}')) {
      return true;
    }

    std::unordered_set<std::string> seen_keys;
    for (;;) {
      std::string key;
      skip_ws();
      if (!parse_string(key, error)) {
        return false;
      }
      if (!seen_keys.emplace(key).second) {
        if (error) {
          *error = "duplicate object key: " + key;
        }
        return false;
      }
      skip_ws();
      if (!consume(':')) {
        if (error) {
          *error = "expected ':' after key '" + key + "'";
        }
        return false;
      }
      JsonValue value{};
      if (!parse_value(value, error)) {
        return false;
      }
      out.object_value.emplace_back(std::move(key), std::move(value));

      skip_ws();
      if (consume('}')) {
        return true;
      }
      if (!consume(',')) {
        if (error) {
          *error = "expected ',' or '}' in object at byte " +
                   std::to_string(pos_);
        }
        return false;
      }
    }
  }

  const std::string& text_;
  size_t pos_ = 0;
};

const JsonValue* find_field(const JsonValue& obj, const char* name) {
  for (const auto& [key, value] : obj.object_value) {
    if (key == name) {
      return &value;
    }
  }
  return nullptr;
}

bool require_type(const JsonValue* value,
                  JsonValue::Type type,
                  const std::string& field,
                  LoadResult& out) {
  if (!value) {
    set_error(out, LoadErrorKind::Validation, "missing required field: " + field);
    return false;
  }
  if (value->type != type) {
    set_error(out, LoadErrorKind::Validation, "field '" + field + "' has wrong type");
    return false;
  }
  return true;
}

bool parse_u32(const JsonValue& value,
               const std::string& field,
               std::uint32_t& out_value,
               LoadResult& out) {
  if (value.type != JsonValue::Type::Number ||
      value.number_value < 0 ||
      value.number_value >
          static_cast<std::int64_t>(
              std::numeric_limits<std::uint32_t>::max())) {
    set_error(out,
              LoadErrorKind::Validation,
              "field '" + field + "' must be uint32 integer");
    return false;
  }
  out_value = static_cast<std::uint32_t>(value.number_value);
  return true;
}

bool parse_non_empty_string(const JsonValue& value,
                            const std::string& field,
                            std::string& out_value,
                            LoadResult& out) {
  if (value.type != JsonValue::Type::String) {
    set_error(out, LoadErrorKind::Validation, "field '" + field + "' has wrong type");
    return false;
  }
  if (value.string_value.empty()) {
    set_error(out, LoadErrorKind::Validation, "field '" + field + "' must not be empty");
    return false;
  }
  out_value = value.string_value;
  return true;
}

std::vector<std::string> normalize_combination(
    const JsonValue& combination_value,
    const std::unordered_set<std::string>& known_camera_ids,
    const std::string& field,
    LoadResult& out) {
  std::vector<std::string> normalized{};
  if (combination_value.type != JsonValue::Type::Array) {
    set_error(out,
              LoadErrorKind::Validation,
              "field '" + field + "' must be array");
    return normalized;
  }

  normalized.reserve(combination_value.array_value.size());
  std::unordered_set<std::string> seen_members;
  for (size_t i = 0; i < combination_value.array_value.size(); ++i) {
    const JsonValue& member = combination_value.array_value[i];
    std::string camera_id;
    if (!parse_non_empty_string(
            member,
            field + "[" + std::to_string(i) + "]",
            camera_id,
            out)) {
      return {};
    }
    if (known_camera_ids.find(camera_id) == known_camera_ids.end()) {
      set_error(out,
                LoadErrorKind::Validation,
                "field '" + field + "[" + std::to_string(i) +
                    "]' references unknown camera_id");
      return {};
    }
    if (!seen_members.emplace(camera_id).second) {
      set_error(out,
                LoadErrorKind::Validation,
                "field '" + field + "' contains duplicate camera_id member");
      return {};
    }
    normalized.push_back(std::move(camera_id));
  }

  if (normalized.size() < 2) {
    set_error(out,
              LoadErrorKind::Validation,
              "field '" + field + "' must contain at least two unique camera_ids");
    return {};
  }

  std::sort(normalized.begin(), normalized.end());
  return normalized;
}

} // namespace

LoadResult load_truth_from_adc_json_text(const std::string& text) {
  LoadResult out{};
  JsonValue root{};
  JsonParser parser(text);
  std::string parse_error;
  if (!parser.parse(root, &parse_error)) {
    set_error(out, LoadErrorKind::Parse, parse_error);
    return out;
  }
  if (root.type != JsonValue::Type::Object) {
    set_error(out, LoadErrorKind::Validation, "root must be object");
    return out;
  }

  const JsonValue* schema_version_value = find_field(root, "schema_version");
  if (!require_type(
          schema_version_value,
          JsonValue::Type::Number,
          "schema_version",
          out)) {
    return out;
  }
  std::uint32_t schema_version = 0;
  if (!parse_u32(*schema_version_value, "schema_version", schema_version, out)) {
    return out;
  }
  if (schema_version < ADC::kMinSupportedSchemaVersion ||
      schema_version > ADC::kMaxSupportedSchemaVersion) {
    set_error(out,
              LoadErrorKind::Validation,
              "unsupported schema_version=" + std::to_string(schema_version));
    return out;
  }

  if (const JsonValue* generator = find_field(root, "generator")) {
    if (generator->type != JsonValue::Type::String) {
      set_error(out, LoadErrorKind::Validation, "field 'generator' has wrong type");
      return out;
    }
  }

  const JsonValue* cameras_value = find_field(root, "cameras");
  if (!require_type(cameras_value, JsonValue::Type::Array, "cameras", out)) {
    return out;
  }
  if (cameras_value->array_value.empty()) {
    set_error(out, LoadErrorKind::Validation, "field 'cameras' must not be empty");
    return out;
  }

  std::unordered_set<std::string> known_camera_ids;
  known_camera_ids.reserve(cameras_value->array_value.size());
  for (size_t i = 0; i < cameras_value->array_value.size(); ++i) {
    const JsonValue& camera = cameras_value->array_value[i];
    if (camera.type != JsonValue::Type::Object) {
      set_error(out,
                LoadErrorKind::Validation,
                "field 'cameras[" + std::to_string(i) + "]' must be object");
      return out;
    }
    const JsonValue* camera_id_value = find_field(camera, "camera_id");
    if (!require_type(
            camera_id_value,
            JsonValue::Type::String,
            "cameras[" + std::to_string(i) + "].camera_id",
            out)) {
      return out;
    }
    if (camera_id_value->string_value.empty()) {
      set_error(out,
                LoadErrorKind::Validation,
                "field 'cameras[" + std::to_string(i) +
                    "].camera_id' must not be empty");
      return out;
    }
    if (!known_camera_ids.emplace(camera_id_value->string_value).second) {
      set_error(out,
                LoadErrorKind::Validation,
                "duplicate cameras[].camera_id='" +
                    camera_id_value->string_value + "'");
      return out;
    }
  }

  const JsonValue* support_value = find_field(root, "concurrent_camera_support");
  if (!require_type(
          support_value,
          JsonValue::Type::Object,
          "concurrent_camera_support",
          out)) {
    return out;
  }

  const JsonValue* supported_value = find_field(*support_value, "supported");
  if (!require_type(
          supported_value,
          JsonValue::Type::Bool,
          "concurrent_camera_support.supported",
          out)) {
    return out;
  }
  const bool supported = supported_value->bool_value;

  if (const JsonValue* error_value = find_field(*support_value, "error")) {
    if (error_value->type != JsonValue::Type::String) {
      set_error(out,
                LoadErrorKind::Validation,
                "field 'concurrent_camera_support.error' has wrong type");
      return out;
    }
    set_error(out,
              LoadErrorKind::Validation,
              "concurrent camera support reported query error");
    return out;
  }

  std::uint32_t max_concurrent_cameras = 0;
  bool has_max_concurrent_cameras = false;
  if (const JsonValue* max_value =
          find_field(*support_value, "max_concurrent_cameras")) {
    if (!parse_u32(
            *max_value,
            "concurrent_camera_support.max_concurrent_cameras",
            max_concurrent_cameras,
            out)) {
      return out;
    }
    if (max_concurrent_cameras < 2) {
      set_error(out,
                LoadErrorKind::Validation,
                "field 'concurrent_camera_support.max_concurrent_cameras' must be at least 2");
      return out;
    }
    has_max_concurrent_cameras = true;
  }

  const JsonValue* combinations_value =
      find_field(*support_value, "camera_id_combinations");
  if (!supported) {
    if (combinations_value != nullptr) {
      set_error(out,
                LoadErrorKind::Validation,
                "concurrent_camera_support.supported=false contradicts camera_id_combinations");
      return out;
    }
    out.ok = true;
    out.truth.kind = TruthKind::Unsupported;
    return out;
  }

  if (!require_type(
          combinations_value,
          JsonValue::Type::Array,
          "concurrent_camera_support.camera_id_combinations",
          out)) {
    return out;
  }
  if (combinations_value->array_value.empty()) {
    set_error(out,
              LoadErrorKind::Validation,
              "field 'concurrent_camera_support.camera_id_combinations' must not be empty");
    return out;
  }

  std::unordered_set<std::string> seen_normalized_combinations;
  std::vector<std::vector<std::string>> normalized_combinations;
  normalized_combinations.reserve(combinations_value->array_value.size());
  for (size_t i = 0; i < combinations_value->array_value.size(); ++i) {
    std::vector<std::string> normalized = normalize_combination(
        combinations_value->array_value[i],
        known_camera_ids,
        "concurrent_camera_support.camera_id_combinations[" +
            std::to_string(i) + "]",
        out);
    if (!out.error_message.empty()) {
      return out;
    }
    if (has_max_concurrent_cameras &&
        normalized.size() > max_concurrent_cameras) {
      set_error(out,
                LoadErrorKind::Validation,
                "camera_id_combinations exceeds max_concurrent_cameras");
      return out;
    }
    std::string combination_key;
    for (size_t j = 0; j < normalized.size(); ++j) {
      if (j != 0) {
        combination_key.push_back('\n');
      }
      combination_key += normalized[j];
    }
    if (!seen_normalized_combinations.emplace(combination_key).second) {
      set_error(out,
                LoadErrorKind::Validation,
                "duplicate normalized camera_id_combinations entry");
      return out;
    }
    normalized_combinations.push_back(std::move(normalized));
  }

  out.ok = true;
  out.truth.kind = TruthKind::Supported;
  out.truth.allowed_camera_id_combinations =
      std::move(normalized_combinations);
  return out;
}

LoadResult load_truth_from_adc_json_payload(SpecPatchView payload) {
  if (payload.size_bytes != 0 && payload.data == nullptr) {
    LoadResult out{};
    set_error(out, LoadErrorKind::Validation, "payload view is invalid");
    return out;
  }
  const auto* chars = static_cast<const char*>(payload.data);
  const std::string text =
      payload.size_bytes == 0 ? std::string{} : std::string(chars, chars + payload.size_bytes);
  return load_truth_from_adc_json_text(text);
}

bool requested_camera_id_set_is_allowed(
    const Truth& truth,
    const std::vector<std::string>& requested_camera_ids) noexcept {
  if (truth.kind != TruthKind::Supported || requested_camera_ids.size() < 2) {
    return false;
  }

  std::vector<std::string> normalized_requested = requested_camera_ids;
  std::sort(normalized_requested.begin(), normalized_requested.end());
  if (std::adjacent_find(
          normalized_requested.begin(),
          normalized_requested.end()) != normalized_requested.end()) {
    return false;
  }

  for (const auto& allowed : truth.allowed_camera_id_combinations) {
    if (allowed.size() < normalized_requested.size()) {
      continue;
    }
    if (std::includes(
            allowed.begin(),
            allowed.end(),
            normalized_requested.begin(),
            normalized_requested.end())) {
      return true;
    }
  }
  return false;
}

} // namespace cambang::camera_concurrency
