#include "core/adc_camera_description.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <locale>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cambang::adc_camera_description {
namespace {

// Keep external camera-description input bounded to the established
// concurrency-ingress limits.
constexpr std::size_t kMaxInputBytes = 1u << 20;
constexpr std::size_t kMaxNestingDepth = 64;
constexpr std::size_t kMaxStringBytes = 64u << 10;
constexpr std::size_t kMaxCameraRecords = 1024;
constexpr std::size_t kMaxCombinationCount = 2048;
constexpr std::size_t kMaxCombinationMembers = 64;

void set_error(LoadResult& out, LoadErrorKind kind, std::string message) {
  out.ok = false;
  out.error_kind = kind;
  out.error_message = std::move(message);
  out.state = {};
}

struct JsonValue {
  enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool bool_value = false;
  std::string number_lexeme;
  std::string string_value;
  std::vector<JsonValue> array_value;
  std::vector<std::pair<std::string, JsonValue>> object_value;
};

class JsonParser final {
 public:
  explicit JsonParser(const std::string& text) : text_(text) {}

  bool parse(JsonValue& out, std::string* error) {
    if (text_.size() > kMaxInputBytes) {
      *error = "input exceeds byte limit";
      return false;
    }
    pos_ = 0;
    if (!parse_value(out, error, 0)) return false;
    skip_ws();
    if (pos_ != text_.size()) {
      *error = "unexpected trailing input at byte " + std::to_string(pos_);
      return false;
    }
    return true;
  }

 private:
  void skip_ws() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
  }

  bool consume(char expected) {
    if (pos_ < text_.size() && text_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool match_literal(const char* text) {
    std::size_t length = 0;
    while (text[length] != '\0') ++length;
    if (pos_ + length > text_.size() || text_.compare(pos_, length, text) != 0) return false;
    pos_ += length;
    return true;
  }

  bool parse_value(JsonValue& out, std::string* error, std::size_t depth) {
    skip_ws();
    if (pos_ >= text_.size()) {
      *error = "unexpected end of json";
      return false;
    }
    const char c = text_[pos_];
    if (c == '{') return parse_object(out, error, depth + 1);
    if (c == '[') return parse_array(out, error, depth + 1);
    if (c == '"') {
      out.type = JsonValue::Type::String;
      return parse_string(out.string_value, error);
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      out.type = JsonValue::Type::Number;
      return parse_number(out.number_lexeme, error);
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
    if (match_literal("null")) return true;
    *error = "unexpected token at byte " + std::to_string(pos_);
    return false;
  }

  bool parse_string(std::string& out, std::string* error) {
    if (!consume('"')) {
      *error = "expected string at byte " + std::to_string(pos_);
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
          *error = "unterminated escape in string";
          return false;
        }
        const char escape = text_[pos_++];
        switch (escape) {
          case '"': value.push_back('"'); break;
          case '\\': value.push_back('\\'); break;
          case '/': value.push_back('/'); break;
          case 'b': value.push_back('\b'); break;
          case 'f': value.push_back('\f'); break;
          case 'n': value.push_back('\n'); break;
          case 'r': value.push_back('\r'); break;
          case 't': value.push_back('\t'); break;
          case 'u': {
            const auto codepoint = decode_unicode_escape(error);
            if (!codepoint) return false;
            append_utf8(*codepoint, value);
            break;
          }
          default:
            *error = "invalid escape sequence in string";
            return false;
        }
      } else {
        if (static_cast<unsigned char>(c) < 0x20) {
          *error = "control character in string";
          return false;
        }
        value.push_back(c);
      }
      if (value.size() > kMaxStringBytes) {
        *error = "string exceeds byte limit";
        return false;
      }
    }
    *error = "unterminated string";
    return false;
  }

  std::optional<std::uint32_t> decode_unicode_escape(std::string* error) {
    const auto first = parse_hex_escape(error);
    if (!first) return std::nullopt;
    std::uint32_t codepoint = *first;
    if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
      if (pos_ + 6 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
        *error = "missing low surrogate after high surrogate escape";
        return std::nullopt;
      }
      pos_ += 2;
      const auto low = parse_hex_escape(error);
      if (!low || *low < 0xDC00u || *low > 0xDFFFu) {
        *error = "invalid low surrogate escape in string";
        return std::nullopt;
      }
      codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) + (*low - 0xDC00u);
    } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
      *error = "unexpected low surrogate escape in string";
      return std::nullopt;
    }
    return codepoint;
  }

  std::optional<std::uint32_t> parse_hex_escape(std::string* error) {
    if (pos_ + 4 > text_.size()) {
      *error = "incomplete unicode escape in string";
      return std::nullopt;
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_++];
      value <<= 4;
      if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
      else {
        *error = "invalid unicode escape in string";
        return std::nullopt;
      }
    }
    return value;
  }

  static void append_utf8(std::uint32_t codepoint, std::string& out) {
    if (codepoint <= 0x7Fu) out.push_back(static_cast<char>(codepoint));
    else if (codepoint <= 0x7FFu) {
      out.push_back(static_cast<char>(0xC0u | (codepoint >> 6)));
      out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint <= 0xFFFFu) {
      out.push_back(static_cast<char>(0xE0u | (codepoint >> 12)));
      out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else {
      out.push_back(static_cast<char>(0xF0u | (codepoint >> 18)));
      out.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    }
  }

  bool parse_number(std::string& out, std::string* error) {
    const std::size_t start = pos_;
    if (consume('-') && pos_ >= text_.size()) return invalid_number(start, error);
    if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
      return invalid_number(start, error);
    }
    if (text_[pos_] == '0') {
      ++pos_;
      if (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        *error = "leading zero is not allowed for numbers";
        return false;
      }
    } else {
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        return invalid_number(start, error);
      }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        return invalid_number(start, error);
      }
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    out.assign(text_.data() + start, pos_ - start);
    return true;
  }

  bool invalid_number(std::size_t start, std::string* error) {
    *error = "invalid number at byte " + std::to_string(start);
    return false;
  }

  bool parse_array(JsonValue& out, std::string* error, std::size_t depth) {
    if (depth > kMaxNestingDepth) {
      *error = "json nesting exceeds depth limit";
      return false;
    }
    if (!consume('[')) {
      *error = "expected '[' at byte " + std::to_string(pos_);
      return false;
    }
    out = {};
    out.type = JsonValue::Type::Array;
    skip_ws();
    if (consume(']')) return true;
    for (;;) {
      JsonValue value;
      if (!parse_value(value, error, depth)) return false;
      out.array_value.push_back(std::move(value));
      skip_ws();
      if (consume(']')) return true;
      if (!consume(',')) {
        *error = "expected ',' or ']' in array at byte " + std::to_string(pos_);
        return false;
      }
    }
  }

  bool parse_object(JsonValue& out, std::string* error, std::size_t depth) {
    if (depth > kMaxNestingDepth) {
      *error = "json nesting exceeds depth limit";
      return false;
    }
    if (!consume('{')) {
      *error = "expected '{' at byte " + std::to_string(pos_);
      return false;
    }
    out = {};
    out.type = JsonValue::Type::Object;
    skip_ws();
    if (consume('}')) return true;
    std::unordered_set<std::string> seen_keys;
    for (;;) {
      std::string key;
      skip_ws();
      if (!parse_string(key, error)) return false;
      if (!seen_keys.emplace(key).second) {
        *error = "duplicate object key: " + key;
        return false;
      }
      skip_ws();
      if (!consume(':')) {
        *error = "expected ':' after key '" + key + "'";
        return false;
      }
      JsonValue value;
      if (!parse_value(value, error, depth)) return false;
      out.object_value.emplace_back(std::move(key), std::move(value));
      skip_ws();
      if (consume('}')) return true;
      if (!consume(',')) {
        *error = "expected ',' or '}' in object at byte " + std::to_string(pos_);
        return false;
      }
    }
  }

  const std::string& text_;
  std::size_t pos_ = 0;
};

const JsonValue* field(const JsonValue& object, std::string_view name) {
  for (const auto& [key, value] : object.object_value) {
    if (key == name) return &value;
  }
  return nullptr;
}

bool require_type(const JsonValue* value, JsonValue::Type type, std::string_view name, LoadResult& out) {
  if (!value) {
    set_error(out, LoadErrorKind::Validation, "missing required field: " + std::string(name));
    return false;
  }
  if (value->type != type) {
    set_error(out, LoadErrorKind::Validation, "field '" + std::string(name) + "' has wrong type");
    return false;
  }
  return true;
}

bool parse_non_empty_string(const JsonValue& value, std::string_view name, std::string& parsed, LoadResult& out) {
  if (value.type != JsonValue::Type::String || value.string_value.empty()) {
    set_error(out, LoadErrorKind::Validation, "field '" + std::string(name) + "' must be non-empty string");
    return false;
  }
  parsed = value.string_value;
  return true;
}

bool parse_uint32(const JsonValue& value, std::string_view name, std::uint32_t& parsed, LoadResult& out) {
  if (value.type != JsonValue::Type::Number || value.number_lexeme.find_first_of(".eE-") != std::string::npos) {
    set_error(out, LoadErrorKind::Validation, "field '" + std::string(name) + "' must be uint32 integer");
    return false;
  }
  const char* begin = value.number_lexeme.data();
  const char* end = begin + value.number_lexeme.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) {
    set_error(out, LoadErrorKind::Validation, "field '" + std::string(name) + "' must be uint32 integer");
    return false;
  }
  return true;
}

bool parse_finite_number(const JsonValue& value, std::string_view name, double& parsed, LoadResult& out) {
  if (value.type != JsonValue::Type::Number) {
    set_error(out, LoadErrorKind::Validation, "field '" + std::string(name) + "' must be number");
    return false;
  }
  std::istringstream input(value.number_lexeme);
  input.imbue(std::locale::classic());
  input >> std::noskipws >> parsed;
  const bool lexeme_has_non_zero_digit =
      value.number_lexeme.find_first_of("123456789") != std::string::npos;
  if (input.fail() || input.peek() != std::char_traits<char>::eof() ||
      !std::isfinite(parsed) || (parsed == 0.0 && lexeme_has_non_zero_digit)) {
    set_error(out, LoadErrorKind::Validation, "field '" + std::string(name) + "' must be finite number");
    return false;
  }
  return true;
}

bool parse_origin(const JsonValue& value, std::string_view name, FactOrigin& origin, LoadResult& out) {
  std::string token;
  if (!parse_non_empty_string(value, name, token, out)) return false;
  if (token == "native_reported") origin = FactOrigin::NATIVE_REPORTED;
  else if (token == "user_supplied") origin = FactOrigin::USER_SUPPLIED;
  else if (token == "derived") origin = FactOrigin::DERIVED;
  else if (token == "virtual_camera_authored") origin = FactOrigin::VIRTUAL_CAMERA_AUTHORED;
  else if (token == "unknown") origin = FactOrigin::UNKNOWN;
  else {
    set_error(out, LoadErrorKind::Validation, "field '" + std::string(name) + "' has unknown fact source");
    return false;
  }
  return true;
}

bool parse_coordinate_domain(const JsonValue& object, CoordinateDomain& domain, LoadResult& out) {
  const JsonValue* value = field(object, "coordinate_domain");
  if (!require_type(value, JsonValue::Type::String, "coordinate_domain", out)) return false;
  const JsonValue* platform_token = field(object, "platform_defined_domain");
  if (value->string_value == "android_sensor_pre_correction_active_array") {
    if (platform_token) {
      set_error(out, LoadErrorKind::Validation, "platform_defined_domain is forbidden for known coordinate domain");
      return false;
    }
    domain = CoordinateDomainAndroidSensorPreCorrectionActiveArray{};
  } else if (value->string_value == "android_sensor_active_array") {
    if (platform_token) {
      set_error(out, LoadErrorKind::Validation, "platform_defined_domain is forbidden for known coordinate domain");
      return false;
    }
    domain = CoordinateDomainAndroidSensorActiveArray{};
  } else if (value->string_value == "delivered_image") {
    if (platform_token) {
      set_error(out, LoadErrorKind::Validation, "platform_defined_domain is forbidden for known coordinate domain");
      return false;
    }
    domain = CoordinateDomainDeliveredImage{};
  } else if (value->string_value == "platform_defined") {
    if (!require_type(platform_token, JsonValue::Type::String, "platform_defined_domain", out)) return false;
    const auto checked = CoordinateDomainPlatformDefined::create(platform_token->string_value);
    if (!checked) {
      set_error(out, LoadErrorKind::Validation, "platform_defined_domain must not be empty");
      return false;
    }
    domain = *checked;
  } else {
    set_error(out, LoadErrorKind::Validation, "coordinate_domain has unknown token");
    return false;
  }
  return true;
}

bool parse_image_state(const JsonValue& value, DistortionImageState& state, LoadResult& out) {
  if (value.type != JsonValue::Type::String) {
    set_error(out, LoadErrorKind::Validation, "image_state has wrong type");
    return false;
  }
  if (value.string_value == "distorted") state = DistortionImageState::DISTORTED;
  else if (value.string_value == "rectified") state = DistortionImageState::RECTIFIED;
  else if (value.string_value == "unknown") state = DistortionImageState::UNKNOWN;
  else {
    set_error(out, LoadErrorKind::Validation, "image_state has unknown token");
    return false;
  }
  return true;
}

bool parse_facing(const JsonValue& object, std::optional<SourcedFact<CameraFacing>>& result, LoadResult& out) {
  const JsonValue* source = field(object, "source");
  const JsonValue* value = field(object, "value");
  if (!require_type(source, JsonValue::Type::String, "facing.source", out) ||
      !require_type(value, JsonValue::Type::String, "facing.value", out)) return false;
  FactOrigin origin;
  if (!parse_origin(*source, "facing.source", origin, out)) return false;
  CameraFacing facing;
  if (value->string_value == "front") facing = CameraFacing::FRONT;
  else if (value->string_value == "back") facing = CameraFacing::BACK;
  else if (value->string_value == "external") facing = CameraFacing::EXTERNAL;
  else if (value->string_value == "unknown") facing = CameraFacing::UNKNOWN;
  else {
    set_error(out, LoadErrorKind::Validation, "facing.value has unknown token");
    return false;
  }
  result = SourcedFact<CameraFacing>{facing, origin};
  return true;
}

bool parse_nature(const JsonValue& object, std::optional<SourcedFact<CameraNature>>& result, LoadResult& out) {
  const JsonValue* source = field(object, "source");
  const JsonValue* value = field(object, "value");
  if (!require_type(source, JsonValue::Type::String, "camera_nature.source", out) ||
      !require_type(value, JsonValue::Type::String, "camera_nature.value", out)) return false;
  FactOrigin origin;
  if (!parse_origin(*source, "camera_nature.source", origin, out)) return false;
  CameraNature nature;
  if (value->string_value == "physical") nature = CameraNature::PHYSICAL;
  else if (value->string_value == "virtual") nature = CameraNature::VIRTUAL;
  else if (value->string_value == "hybrid") nature = CameraNature::HYBRID;
  else if (value->string_value == "unknown") nature = CameraNature::UNKNOWN;
  else {
    set_error(out, LoadErrorKind::Validation, "camera_nature.value has unknown token");
    return false;
  }
  result = SourcedFact<CameraNature>{nature, origin};
  return true;
}

bool parse_sensor_orientation(const JsonValue& object,
                              std::optional<SourcedFact<SensorOrientationDegrees>>& result,
                              LoadResult& out) {
  const JsonValue* source = field(object, "source");
  const JsonValue* value = field(object, "value_degrees");
  if (!require_type(source, JsonValue::Type::String, "sensor_orientation.source", out) ||
      !require_type(value, JsonValue::Type::Number, "sensor_orientation.value_degrees", out)) return false;
  FactOrigin origin;
  std::uint32_t degrees = 0;
  if (!parse_origin(*source, "sensor_orientation.source", origin, out) ||
      !parse_uint32(*value, "sensor_orientation.value_degrees", degrees, out)) return false;
  SensorOrientationDegrees orientation;
  switch (degrees) {
    case 0: orientation = SensorOrientationDegrees::DEGREES_0; break;
    case 90: orientation = SensorOrientationDegrees::DEGREES_90; break;
    case 180: orientation = SensorOrientationDegrees::DEGREES_180; break;
    case 270: orientation = SensorOrientationDegrees::DEGREES_270; break;
    default:
      set_error(out, LoadErrorKind::Validation, "sensor_orientation.value_degrees has unknown token");
      return false;
  }
  result = SourcedFact<SensorOrientationDegrees>{orientation, origin};
  return true;
}

bool parse_intrinsics(const JsonValue& object, std::optional<SourcedFact<Intrinsics>>& result, LoadResult& out) {
  const JsonValue* source = field(object, "source");
  const JsonValue* fx = field(object, "focal_length_x_px");
  const JsonValue* fy = field(object, "focal_length_y_px");
  const JsonValue* cx = field(object, "principal_point_x_px");
  const JsonValue* cy = field(object, "principal_point_y_px");
  const JsonValue* width = field(object, "reference_width_px");
  const JsonValue* height = field(object, "reference_height_px");
  if (!require_type(source, JsonValue::Type::String, "intrinsics.source", out) ||
      !require_type(fx, JsonValue::Type::Number, "intrinsics.focal_length_x_px", out) ||
      !require_type(fy, JsonValue::Type::Number, "intrinsics.focal_length_y_px", out) ||
      !require_type(cx, JsonValue::Type::Number, "intrinsics.principal_point_x_px", out) ||
      !require_type(cy, JsonValue::Type::Number, "intrinsics.principal_point_y_px", out) ||
      !require_type(width, JsonValue::Type::Number, "intrinsics.reference_width_px", out) ||
      !require_type(height, JsonValue::Type::Number, "intrinsics.reference_height_px", out)) return false;
  FactOrigin origin;
  double fx_value, fy_value, cx_value, cy_value;
  std::uint32_t width_value, height_value;
  if (!parse_origin(*source, "intrinsics.source", origin, out) ||
      !parse_finite_number(*fx, "intrinsics.focal_length_x_px", fx_value, out) ||
      !parse_finite_number(*fy, "intrinsics.focal_length_y_px", fy_value, out) ||
      !parse_finite_number(*cx, "intrinsics.principal_point_x_px", cx_value, out) ||
      !parse_finite_number(*cy, "intrinsics.principal_point_y_px", cy_value, out) ||
      !parse_uint32(*width, "intrinsics.reference_width_px", width_value, out) ||
      !parse_uint32(*height, "intrinsics.reference_height_px", height_value, out)) return false;
  std::optional<double> skew;
  if (const JsonValue* value = field(object, "skew_px")) {
    double parsed = 0.0;
    if (!parse_finite_number(*value, "intrinsics.skew_px", parsed, out)) return false;
    skew = parsed;
  }
  CoordinateDomain domain;
  if (!parse_coordinate_domain(object, domain, out)) return false;
  const auto checked = Intrinsics::create(fx_value, fy_value, cx_value, cy_value, skew, width_value, height_value, std::move(domain));
  if (!checked) {
    set_error(out, LoadErrorKind::Validation, "intrinsics is invalid");
    return false;
  }
  result = SourcedFact<Intrinsics>{*checked, origin};
  return true;
}

bool has_field(const JsonValue& object, std::string_view name) { return field(object, name) != nullptr; }

bool parse_distortion(const JsonValue& object, std::optional<SourcedFact<Distortion>>& result, LoadResult& out) {
  const JsonValue* source = field(object, "source");
  const JsonValue* model = field(object, "model");
  const JsonValue* image_state = field(object, "image_state");
  if (!require_type(source, JsonValue::Type::String, "distortion.source", out) ||
      !require_type(model, JsonValue::Type::String, "distortion.model", out) ||
      !require_type(image_state, JsonValue::Type::String, "distortion.image_state", out)) return false;
  FactOrigin origin;
  DistortionImageState state;
  if (!parse_origin(*source, "distortion.source", origin, out) || !parse_image_state(*image_state, state, out)) return false;
  if (model->string_value == "none") {
    for (const char* forbidden : {"radial_k1", "radial_k2", "radial_k3", "tangential_p1", "tangential_p2", "reference_width_px", "reference_height_px", "coordinate_domain", "platform_defined_domain"}) {
      if (has_field(object, forbidden)) {
        set_error(out, LoadErrorKind::Validation, "distortion.none contains model-specific field");
        return false;
      }
    }
    result = SourcedFact<Distortion>{NoDistortion{state}, origin};
    return true;
  }
  if (model->string_value != "brown_conrady_5") {
    set_error(out, LoadErrorKind::Validation, "distortion.model has unknown token");
    return false;
  }
  const JsonValue* k1 = field(object, "radial_k1");
  const JsonValue* k2 = field(object, "radial_k2");
  const JsonValue* k3 = field(object, "radial_k3");
  const JsonValue* p1 = field(object, "tangential_p1");
  const JsonValue* p2 = field(object, "tangential_p2");
  const JsonValue* width = field(object, "reference_width_px");
  const JsonValue* height = field(object, "reference_height_px");
  if (!require_type(k1, JsonValue::Type::Number, "distortion.radial_k1", out) ||
      !require_type(k2, JsonValue::Type::Number, "distortion.radial_k2", out) ||
      !require_type(k3, JsonValue::Type::Number, "distortion.radial_k3", out) ||
      !require_type(p1, JsonValue::Type::Number, "distortion.tangential_p1", out) ||
      !require_type(p2, JsonValue::Type::Number, "distortion.tangential_p2", out) ||
      !require_type(width, JsonValue::Type::Number, "distortion.reference_width_px", out) ||
      !require_type(height, JsonValue::Type::Number, "distortion.reference_height_px", out)) return false;
  double k1_value, k2_value, k3_value, p1_value, p2_value;
  std::uint32_t width_value, height_value;
  if (!parse_finite_number(*k1, "distortion.radial_k1", k1_value, out) ||
      !parse_finite_number(*k2, "distortion.radial_k2", k2_value, out) ||
      !parse_finite_number(*k3, "distortion.radial_k3", k3_value, out) ||
      !parse_finite_number(*p1, "distortion.tangential_p1", p1_value, out) ||
      !parse_finite_number(*p2, "distortion.tangential_p2", p2_value, out) ||
      !parse_uint32(*width, "distortion.reference_width_px", width_value, out) ||
      !parse_uint32(*height, "distortion.reference_height_px", height_value, out)) return false;
  CoordinateDomain domain;
  if (!parse_coordinate_domain(object, domain, out)) return false;
  const auto checked = BrownConrady5Distortion::create(k1_value, k2_value, k3_value, p1_value, p2_value, width_value, height_value, std::move(domain), state);
  if (!checked) {
    set_error(out, LoadErrorKind::Validation, "brown_conrady_5 distortion is invalid");
    return false;
  }
  result = SourcedFact<Distortion>{Distortion{*checked}, origin};
  return true;
}

bool parse_vec3(const JsonValue& value, std::string_view name, Vec3Meters& parsed, LoadResult& out) {
  if (value.type != JsonValue::Type::Array || value.array_value.size() != 3) {
    set_error(out, LoadErrorKind::Validation, "field '" + std::string(name) + "' must be three finite numbers");
    return false;
  }
  return parse_finite_number(value.array_value[0], std::string(name) + "[0]", parsed.x, out) &&
         parse_finite_number(value.array_value[1], std::string(name) + "[1]", parsed.y, out) &&
         parse_finite_number(value.array_value[2], std::string(name) + "[2]", parsed.z, out);
}

bool parse_quaternion(const JsonValue& value, QuaternionXyzw& parsed, LoadResult& out) {
  if (value.type != JsonValue::Type::Array || value.array_value.size() != 4) {
    set_error(out, LoadErrorKind::Validation, "rotation_xyzw must be four finite numbers");
    return false;
  }
  return parse_finite_number(value.array_value[0], "rotation_xyzw[0]", parsed.x, out) &&
         parse_finite_number(value.array_value[1], "rotation_xyzw[1]", parsed.y, out) &&
         parse_finite_number(value.array_value[2], "rotation_xyzw[2]", parsed.z, out) &&
         parse_finite_number(value.array_value[3], "rotation_xyzw[3]", parsed.w, out);
}

bool require_absent(const JsonValue& object, std::string_view name, LoadResult& out) {
  if (field(object, name)) {
    set_error(out, LoadErrorKind::Validation, "field '" + std::string(name) + "' is forbidden by pose reference kind");
    return false;
  }
  return true;
}

bool parse_pose(const JsonValue& object, const std::string& camera_id, std::optional<SourcedFact<CameraPose>>& result, LoadResult& out) {
  const JsonValue* source = field(object, "source");
  const JsonValue* reference_kind = field(object, "reference_kind");
  const JsonValue* convention = field(object, "coordinate_convention");
  const JsonValue* translation = field(object, "translation_m");
  const JsonValue* rotation = field(object, "rotation_xyzw");
  if (!require_type(source, JsonValue::Type::String, "pose.source", out) ||
      !require_type(reference_kind, JsonValue::Type::String, "pose.reference_kind", out) ||
      !require_type(convention, JsonValue::Type::String, "pose.coordinate_convention", out) ||
      !require_type(translation, JsonValue::Type::Array, "pose.translation_m", out) ||
      !require_type(rotation, JsonValue::Type::Array, "pose.rotation_xyzw", out)) return false;
  FactOrigin origin;
  if (!parse_origin(*source, "pose.source", origin, out)) return false;
  std::optional<PoseReference> reference;
  const JsonValue* camera_reference = field(object, "reference_camera_id");
  const JsonValue* custom_reference = field(object, "reference_id");
  const JsonValue* platform_reference = field(object, "platform_defined_reference");
  if (reference_kind->string_value == "camera") {
    if (!require_type(camera_reference, JsonValue::Type::String, "pose.reference_camera_id", out) ||
        !require_absent(object, "reference_id", out) || !require_absent(object, "platform_defined_reference", out)) return false;
    const auto checked = PoseReferenceCamera::create(camera_reference->string_value);
    if (!checked || checked->camera_id() == camera_id) {
      set_error(out, LoadErrorKind::Validation, "pose camera reference must be non-empty and not self");
      return false;
    }
    reference = *checked;
  } else if (reference_kind->string_value == "custom_reference") {
    if (!require_type(custom_reference, JsonValue::Type::String, "pose.reference_id", out) ||
        !require_absent(object, "reference_camera_id", out) || !require_absent(object, "platform_defined_reference", out)) return false;
    const auto checked = PoseReferenceCustom::create(custom_reference->string_value);
    if (!checked) {
      set_error(out, LoadErrorKind::Validation, "pose reference_id must not be empty");
      return false;
    }
    reference = *checked;
  } else if (reference_kind->string_value == "platform_defined") {
    if (!require_type(platform_reference, JsonValue::Type::String, "pose.platform_defined_reference", out) ||
        !require_absent(object, "reference_camera_id", out) || !require_absent(object, "reference_id", out)) return false;
    const auto checked = PoseReferencePlatformDefined::create(platform_reference->string_value);
    if (!checked) {
      set_error(out, LoadErrorKind::Validation, "pose platform_defined_reference must not be empty");
      return false;
    }
    reference = *checked;
  } else {
    if (!require_absent(object, "reference_camera_id", out) ||
        !require_absent(object, "reference_id", out) || !require_absent(object, "platform_defined_reference", out)) return false;
    if (reference_kind->string_value == "primary_camera") reference = PoseReferencePrimaryCamera{};
    else if (reference_kind->string_value == "device_motion_sensor") reference = PoseReferenceDeviceMotionSensor{};
    else if (reference_kind->string_value == "automotive_reference") reference = PoseReferenceAutomotive{};
    else if (reference_kind->string_value == "unknown") reference = PoseReferenceUnknown{};
    else {
      set_error(out, LoadErrorKind::Validation, "pose.reference_kind has unknown token");
      return false;
    }
  }
  PoseConvention parsed_convention;
  const JsonValue* platform_convention = field(object, "platform_defined_convention");
  if (convention->string_value == "android_camera2") {
    if (platform_convention) {
      set_error(out, LoadErrorKind::Validation, "platform_defined_convention is forbidden for known convention");
      return false;
    }
    parsed_convention = PoseConventionAndroidCamera2{};
  } else if (convention->string_value == "camera_optical_frame") {
    if (platform_convention) {
      set_error(out, LoadErrorKind::Validation, "platform_defined_convention is forbidden for known convention");
      return false;
    }
    parsed_convention = PoseConventionCameraOpticalFrame{};
  } else if (convention->string_value == "platform_defined") {
    if (!require_type(platform_convention, JsonValue::Type::String, "pose.platform_defined_convention", out)) return false;
    const auto checked = PoseConventionPlatformDefined::create(platform_convention->string_value);
    if (!checked) {
      set_error(out, LoadErrorKind::Validation, "pose platform_defined_convention must not be empty");
      return false;
    }
    parsed_convention = *checked;
  } else {
    set_error(out, LoadErrorKind::Validation, "pose.coordinate_convention has unknown token");
    return false;
  }
  Vec3Meters translation_value{};
  QuaternionXyzw rotation_value{};
  if (!parse_vec3(*translation, "pose.translation_m", translation_value, out) ||
      !parse_quaternion(*rotation, rotation_value, out)) return false;
  const auto checked = CameraPose::create(std::move(*reference), std::move(parsed_convention), translation_value, rotation_value);
  if (!checked) {
    set_error(out, LoadErrorKind::Validation, "pose has non-finite or zero quaternion");
    return false;
  }
  result = SourcedFact<CameraPose>{*checked, origin};
  return true;
}

bool parse_camera(const JsonValue& value, ExternalCameraDescriptionEntry& entry, LoadResult& out) {
  if (value.type != JsonValue::Type::Object) {
    set_error(out, LoadErrorKind::Validation, "camera entry must be object");
    return false;
  }
  const JsonValue* id = field(value, "camera_id");
  if (!require_type(id, JsonValue::Type::String, "camera_id", out) || id->string_value.empty()) {
    if (out.error_message.empty()) set_error(out, LoadErrorKind::Validation, "camera_id must not be empty");
    return false;
  }
  entry.camera_id = id->string_value;
  if (const JsonValue* facing = field(value, "facing")) {
    if (facing->type != JsonValue::Type::Object || !parse_facing(*facing, entry.facts.facing, out)) return false;
  }
  if (const JsonValue* nature = field(value, "camera_nature")) {
    if (nature->type != JsonValue::Type::Object || !parse_nature(*nature, entry.facts.nature, out)) return false;
  }
  if (const JsonValue* orientation = field(value, "sensor_orientation")) {
    if (orientation->type != JsonValue::Type::Object || !parse_sensor_orientation(*orientation, entry.facts.sensor_orientation, out)) return false;
  }
  if (const JsonValue* intrinsics = field(value, "intrinsics")) {
    if (intrinsics->type != JsonValue::Type::Object || !parse_intrinsics(*intrinsics, entry.facts.intrinsics, out)) return false;
  }
  if (const JsonValue* distortion = field(value, "distortion")) {
    if (distortion->type != JsonValue::Type::Object || !parse_distortion(*distortion, entry.facts.distortion, out)) return false;
  }
  if (const JsonValue* pose = field(value, "pose")) {
    if (pose->type != JsonValue::Type::Object || !parse_pose(*pose, entry.camera_id, entry.facts.pose, out)) return false;
  }
  return true;
}

bool parse_concurrency(const JsonValue& value,
                       const ExternalCameraDescriptionState::Entries& entries,
                       std::optional<camera_concurrency::Truth>& result,
                       LoadResult& out) {
  if (value.type != JsonValue::Type::Object) {
    set_error(out, LoadErrorKind::Validation, "concurrent_camera_support must be object");
    return false;
  }
  const JsonValue* supported = field(value, "supported");
  if (!require_type(supported, JsonValue::Type::Bool, "concurrent_camera_support.supported", out)) return false;
  const JsonValue* combinations = field(value, "camera_id_combinations");
  if (!supported->bool_value) {
    if (combinations && (combinations->type != JsonValue::Type::Array || !combinations->array_value.empty())) {
      set_error(out, LoadErrorKind::Validation, "supported=false contradicts camera_id_combinations");
      return false;
    }
    result = camera_concurrency::Truth{camera_concurrency::TruthKind::Unsupported, {}};
    return true;
  }
  if (!require_type(combinations, JsonValue::Type::Array, "concurrent_camera_support.camera_id_combinations", out) ||
      combinations->array_value.empty() || combinations->array_value.size() > kMaxCombinationCount) {
    if (out.error_message.empty()) set_error(out, LoadErrorKind::Validation, "camera_id_combinations has invalid count");
    return false;
  }
  std::vector<std::vector<std::string>> normalized;
  std::unordered_set<std::string> seen_combinations;
  for (const JsonValue& combination : combinations->array_value) {
    if (combination.type != JsonValue::Type::Array || combination.array_value.size() < 2 ||
        combination.array_value.size() > kMaxCombinationMembers) {
      set_error(out, LoadErrorKind::Validation, "camera_id combination has invalid member count");
      return false;
    }
    std::vector<std::string> ids;
    std::unordered_set<std::string> seen_ids;
    for (const JsonValue& member : combination.array_value) {
      std::string id;
      if (!parse_non_empty_string(member, "camera_id_combinations member", id, out)) return false;
      if (entries.find(id) == entries.end()) {
        set_error(out, LoadErrorKind::Validation, "camera_id combination references unknown camera_id");
        return false;
      }
      if (!seen_ids.emplace(id).second) {
        set_error(out, LoadErrorKind::Validation, "camera_id combination contains duplicate camera_id");
        return false;
      }
      ids.push_back(std::move(id));
    }
    std::sort(ids.begin(), ids.end());
    std::string key;
    for (const std::string& id : ids) key += '\n' + id;
    if (!seen_combinations.emplace(key).second) {
      set_error(out, LoadErrorKind::Validation, "duplicate normalized camera_id combination");
      return false;
    }
    normalized.push_back(std::move(ids));
  }
  result = camera_concurrency::Truth{camera_concurrency::TruthKind::Supported, std::move(normalized)};
  return true;
}

bool validate_provenance(const JsonValue& root, LoadResult& out) {
  for (const char* name : {"generator", "generator_version", "device_model", "device_manufacturer", "android_version", "godot_version"}) {
    if (const JsonValue* value = field(root, name); value && value->type != JsonValue::Type::String) {
      set_error(out, LoadErrorKind::Validation, std::string("field '") + name + "' has wrong type");
      return false;
    }
  }
  if (const JsonValue* timestamp = field(root, "timestamp_ms")) {
    if (timestamp->type != JsonValue::Type::Number ||
        timestamp->number_lexeme.find_first_of(".eE-") != std::string::npos) {
      set_error(out, LoadErrorKind::Validation, "field 'timestamp_ms' must be non-negative integer");
      return false;
    }
  }
  return true;
}

} // namespace

std::size_t max_supported_input_bytes() noexcept { return kMaxInputBytes; }
std::size_t max_supported_nesting_depth() noexcept { return kMaxNestingDepth; }
std::size_t max_supported_string_bytes() noexcept { return kMaxStringBytes; }
std::size_t max_supported_camera_records() noexcept { return kMaxCameraRecords; }
std::size_t max_supported_combination_count() noexcept { return kMaxCombinationCount; }
std::size_t max_supported_combination_members() noexcept { return kMaxCombinationMembers; }

LoadResult load_replacement_from_json_text(const std::string& text) {
  LoadResult out;
  JsonValue root;
  JsonParser parser(text);
  std::string parse_error;
  if (!parser.parse(root, &parse_error)) {
    set_error(out, LoadErrorKind::Parse, std::move(parse_error));
    return out;
  }
  if (root.type != JsonValue::Type::Object) {
    set_error(out, LoadErrorKind::Validation, "root must be object");
    return out;
  }
  const JsonValue* version = field(root, "schema_version");
  std::uint32_t parsed_version = 0;
  if (!require_type(version, JsonValue::Type::Number, "schema_version", out) ||
      !parse_uint32(*version, "schema_version", parsed_version, out)) return out;
  if (parsed_version < ADC::kMinSupportedSchemaVersion || parsed_version > ADC::kMaxSupportedSchemaVersion) {
    set_error(out, LoadErrorKind::Validation, "unsupported schema_version");
    return out;
  }
  if (!validate_provenance(root, out)) return out;
  const JsonValue* cameras = field(root, "cameras");
  if (!require_type(cameras, JsonValue::Type::Array, "cameras", out) || cameras->array_value.size() > kMaxCameraRecords) {
    if (out.error_message.empty()) set_error(out, LoadErrorKind::Validation, "cameras exceeds supported count limit");
    return out;
  }
  ExternalCameraDescriptionState::Entries entries;
  entries.reserve(cameras->array_value.size());
  for (const JsonValue& camera : cameras->array_value) {
    ExternalCameraDescriptionEntry entry;
    if (!parse_camera(camera, entry, out)) return out;
    const std::string id = entry.camera_id;
    if (!entries.emplace(id, std::move(entry)).second) {
      set_error(out, LoadErrorKind::Validation, "duplicate cameras[].camera_id");
      return out;
    }
  }
  std::optional<camera_concurrency::Truth> concurrency;
  if (const JsonValue* support = field(root, "concurrent_camera_support")) {
    if (!parse_concurrency(*support, entries, concurrency, out)) return out;
  }
  out.state.replace(std::move(entries), std::move(concurrency));
  out.ok = true;
  return out;
}

} // namespace cambang::adc_camera_description
