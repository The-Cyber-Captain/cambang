#include "imaging/synthetic/scenario_loader_parse.h"

#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>

namespace cambang {
namespace {

void set_error(std::string* error, const std::string& message) {
  if (error) {
    *error = message;
  }
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
  std::string string_value;
  std::vector<JsonValue> array_value;
  std::vector<std::pair<std::string, JsonValue>> object_value;
};

class JsonParser {
public:
  explicit JsonParser(const std::string& text) : text_(text) {}

  bool parse(JsonValue& out, std::string* error) {
    pos_ = 0;
    if (!parse_value(out, error)) {
      return false;
    }
    skip_ws();
    if (pos_ != text_.size()) {
      set_error(error, "unexpected trailing input at byte " + std::to_string(pos_));
      return false;
    }
    return true;
  }

private:
  void skip_ws() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
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

  bool parse_value(JsonValue& out, std::string* error) {
    skip_ws();
    if (pos_ >= text_.size()) {
      set_error(error, "unexpected end of json");
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

    set_error(error, "unexpected token at byte " + std::to_string(pos_));
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

  bool parse_string(std::string& out, std::string* error) {
    if (!consume('"')) {
      set_error(error, "expected string at byte " + std::to_string(pos_));
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
          set_error(error, "unterminated escape in string");
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
              set_error(error, "incomplete unicode escape in string");
              return false;
            }
            // Strict v1 parser accepts \uXXXX escapes but only preserves ASCII
            // code points directly; non-ASCII is rejected to keep parser minimal.
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
                set_error(error, "invalid unicode escape in string");
                return false;
              }
            }
            if (code > 0x7F) {
              set_error(error, "non-ascii unicode escape is not supported in strict v1 parser");
              return false;
            }
            value.push_back(static_cast<char>(code));
            break;
          }
          default:
            set_error(error, "invalid escape sequence in string");
            return false;
        }
      } else {
        if (static_cast<unsigned char>(c) < 0x20) {
          set_error(error, "control character in string");
          return false;
        }
        value.push_back(c);
      }
    }

    set_error(error, "unterminated string");
    return false;
  }

  bool parse_integer(std::int64_t& out, std::string* error) {
    const size_t start = pos_;
    bool neg = false;
    if (consume('-')) {
      neg = true;
    }
    if (pos_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
      set_error(error, "invalid number at byte " + std::to_string(start));
      return false;
    }
    if (text_[pos_] == '0' && pos_ + 1 < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_ + 1]))) {
      set_error(error, "leading zero is not allowed for numbers");
      return false;
    }

    std::int64_t value = 0;
    while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
      const int digit = text_[pos_] - '0';
      if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
        set_error(error, "integer overflow in number literal");
        return false;
      }
      value = value * 10 + digit;
      ++pos_;
    }

    if (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '.' || c == 'e' || c == 'E') {
        set_error(error, "non-integer numbers are not supported in strict v1 loader");
        return false;
      }
    }

    out = neg ? -value : value;
    return true;
  }

  bool parse_array(JsonValue& out, std::string* error) {
    if (!consume('[')) {
      set_error(error, "expected '[' at byte " + std::to_string(pos_));
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
        set_error(error, "expected ',' or ']' in array at byte " + std::to_string(pos_));
        return false;
      }
    }
  }

  bool parse_object(JsonValue& out, std::string* error) {
    if (!consume('{')) {
      set_error(error, "expected '{' at byte " + std::to_string(pos_));
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
        set_error(error, "duplicate object key: " + key);
        return false;
      }
      skip_ws();
      if (!consume(':')) {
        set_error(error, "expected ':' after key '" + key + "'");
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
        set_error(error, "expected ',' or '}' in object at byte " + std::to_string(pos_));
        return false;
      }
    }
  }

private:
  const std::string& text_;
  size_t pos_ = 0;
};

const JsonValue* find_field(const JsonValue& obj, const char* name) {
  for (const auto& kv : obj.object_value) {
    if (kv.first == name) {
      return &kv.second;
    }
  }
  return nullptr;
}

bool require_only_fields(const JsonValue& obj,
                         const std::initializer_list<const char*>& allowed,
                         std::string* error,
                         const std::string& context) {
  std::unordered_set<std::string> allowed_set;
  allowed_set.reserve(allowed.size());
  for (const auto* name : allowed) {
    allowed_set.emplace(name);
  }
  for (const auto& kv : obj.object_value) {
    if (allowed_set.find(kv.first) == allowed_set.end()) {
      set_error(error, context + " contains unknown field: " + kv.first);
      return false;
    }
  }
  return true;
}

bool require_type(const JsonValue* value, JsonValue::Type type, const std::string& field, std::string* error) {
  if (!value) {
    set_error(error, "missing required field: " + field);
    return false;
  }
  if (value->type != type) {
    set_error(error, "field '" + field + "' has wrong type");
    return false;
  }
  return true;
}

bool parse_u32(const JsonValue& value, const std::string& field, std::uint32_t& out, std::string* error) {
  if (value.type != JsonValue::Type::Number || value.number_value < 0 || value.number_value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
    set_error(error, "field '" + field + "' must be uint32 integer");
    return false;
  }
  out = static_cast<std::uint32_t>(value.number_value);
  return true;
}

bool parse_u64(const JsonValue& value, const std::string& field, std::uint64_t& out, std::string* error) {
  if (value.type != JsonValue::Type::Number || value.number_value < 0) {
    set_error(error, "field '" + field + "' must be uint64 integer");
    return false;
  }
  out = static_cast<std::uint64_t>(value.number_value);
  return true;
}

bool parse_capture_profile(const JsonValue& value,
                           SyntheticScenarioLoaderParsedCaptureProfile& out,
                           std::string* error,
                           const std::string& context) {
  if (value.type != JsonValue::Type::Object) {
    set_error(error, context + " must be object");
    return false;
  }
  if (!require_only_fields(value,
                           {"width", "height", "format_fourcc", "target_fps_min", "target_fps_max"},
                           error,
                           context)) {
    return false;
  }

  const JsonValue* width = find_field(value, "width");
  const JsonValue* height = find_field(value, "height");
  const JsonValue* format = find_field(value, "format_fourcc");
  const JsonValue* fps_min = find_field(value, "target_fps_min");
  const JsonValue* fps_max = find_field(value, "target_fps_max");

  if (!require_type(width, JsonValue::Type::Number, context + ".width", error) ||
      !require_type(height, JsonValue::Type::Number, context + ".height", error) ||
      !require_type(format, JsonValue::Type::Number, context + ".format_fourcc", error) ||
      !require_type(fps_min, JsonValue::Type::Number, context + ".target_fps_min", error) ||
      !require_type(fps_max, JsonValue::Type::Number, context + ".target_fps_max", error)) {
    return false;
  }

  return parse_u32(*width, context + ".width", out.width, error) &&
         parse_u32(*height, context + ".height", out.height, error) &&
         parse_u32(*format, context + ".format_fourcc", out.format_fourcc, error) &&
         parse_u32(*fps_min, context + ".target_fps_min", out.target_fps_min, error) &&
         parse_u32(*fps_max, context + ".target_fps_max", out.target_fps_max, error);
}

bool parse_picture(const JsonValue& value,
                   SyntheticScenarioLoaderParsedPicture& out,
                   std::string* error,
                   const std::string& context) {
  if (value.type != JsonValue::Type::Object) {
    set_error(error, context + " must be object");
    return false;
  }
  if (!require_only_fields(value,
                           {"preset", "seed", "generator_fps_num", "generator_fps_den", "overlay_frame_index_offsets", "overlay_moving_bar", "solid_r", "solid_g", "solid_b", "solid_a", "checker_size_px"},
                           error,
                           context)) {
    return false;
  }

  const JsonValue* preset = find_field(value, "preset");
  const JsonValue* seed = find_field(value, "seed");
  const JsonValue* generator_fps_num = find_field(value, "generator_fps_num");
  const JsonValue* generator_fps_den = find_field(value, "generator_fps_den");
  const JsonValue* overlay_offsets = find_field(value, "overlay_frame_index_offsets");
  const JsonValue* overlay_bar = find_field(value, "overlay_moving_bar");
  const JsonValue* solid_r = find_field(value, "solid_r");
  const JsonValue* solid_g = find_field(value, "solid_g");
  const JsonValue* solid_b = find_field(value, "solid_b");
  const JsonValue* solid_a = find_field(value, "solid_a");
  const JsonValue* checker = find_field(value, "checker_size_px");

  if (!require_type(preset, JsonValue::Type::String, context + ".preset", error) ||
      !require_type(seed, JsonValue::Type::Number, context + ".seed", error) ||
      (generator_fps_num && !require_type(generator_fps_num, JsonValue::Type::Number, context + ".generator_fps_num", error)) ||
      (generator_fps_den && !require_type(generator_fps_den, JsonValue::Type::Number, context + ".generator_fps_den", error)) ||
      !require_type(overlay_offsets, JsonValue::Type::Bool, context + ".overlay_frame_index_offsets", error) ||
      !require_type(overlay_bar, JsonValue::Type::Bool, context + ".overlay_moving_bar", error) ||
      !require_type(solid_r, JsonValue::Type::Number, context + ".solid_r", error) ||
      !require_type(solid_g, JsonValue::Type::Number, context + ".solid_g", error) ||
      !require_type(solid_b, JsonValue::Type::Number, context + ".solid_b", error) ||
      !require_type(solid_a, JsonValue::Type::Number, context + ".solid_a", error) ||
      !require_type(checker, JsonValue::Type::Number, context + ".checker_size_px", error)) {
    return false;
  }

  std::uint32_t r = 0;
  std::uint32_t g = 0;
  std::uint32_t b = 0;
  std::uint32_t a = 0;
  if (!parse_u32(*seed, context + ".seed", out.seed, error) ||
      !parse_u32(*solid_r, context + ".solid_r", r, error) ||
      !parse_u32(*solid_g, context + ".solid_g", g, error) ||
      !parse_u32(*solid_b, context + ".solid_b", b, error) ||
      !parse_u32(*solid_a, context + ".solid_a", a, error) ||
      !parse_u32(*checker, context + ".checker_size_px", out.checker_size_px, error)) {
    return false;
  }
  if (generator_fps_num &&
      !parse_u32(*generator_fps_num, context + ".generator_fps_num", out.generator_fps_num, error)) {
    return false;
  }
  if (generator_fps_den &&
      !parse_u32(*generator_fps_den, context + ".generator_fps_den", out.generator_fps_den, error)) {
    return false;
  }
  if (r > 255 || g > 255 || b > 255 || a > 255) {
    set_error(error, context + " rgba components must be <= 255");
    return false;
  }

  out.preset = preset->string_value;
  out.overlay_frame_index_offsets = overlay_offsets->bool_value;
  out.overlay_moving_bar = overlay_bar->bool_value;
  out.solid_r = static_cast<std::uint8_t>(r);
  out.solid_g = static_cast<std::uint8_t>(g);
  out.solid_b = static_cast<std::uint8_t>(b);
  out.solid_a = static_cast<std::uint8_t>(a);
  return true;
}

bool parse_document_object(const JsonValue& root,
                           SyntheticScenarioLoaderParsedDocument& out,
                           std::string* error) {
  if (root.type != JsonValue::Type::Object) {
    set_error(error, "scenario json root must be object");
    return false;
  }
  if (!require_only_fields(root, {"schema_version", "devices", "streams", "timeline"}, error, "root")) {
    return false;
  }

  const JsonValue* schema_version = find_field(root, "schema_version");
  const JsonValue* devices = find_field(root, "devices");
  const JsonValue* streams = find_field(root, "streams");
  const JsonValue* timeline = find_field(root, "timeline");

  if (!require_type(schema_version, JsonValue::Type::Number, "schema_version", error) ||
      !require_type(devices, JsonValue::Type::Array, "devices", error) ||
      !require_type(streams, JsonValue::Type::Array, "streams", error) ||
      !require_type(timeline, JsonValue::Type::Array, "timeline", error)) {
    return false;
  }
  if (!parse_u32(*schema_version, "schema_version", out.schema_version, error)) {
    return false;
  }

  out = {};
  out.schema_version = static_cast<std::uint32_t>(schema_version->number_value);

  out.devices.reserve(devices->array_value.size());
  for (size_t i = 0; i < devices->array_value.size(); ++i) {
    const auto& item = devices->array_value[i];
    const std::string ctx = "devices[" + std::to_string(i) + "]";
    if (item.type != JsonValue::Type::Object) {
      set_error(error, ctx + " must be object");
      return false;
    }
    if (!require_only_fields(item, {"key", "endpoint_index"}, error, ctx)) {
      return false;
    }
    const JsonValue* key = find_field(item, "key");
    const JsonValue* endpoint = find_field(item, "endpoint_index");
    if (!require_type(key, JsonValue::Type::String, ctx + ".key", error) ||
        !require_type(endpoint, JsonValue::Type::Number, ctx + ".endpoint_index", error)) {
      return false;
    }
    SyntheticScenarioLoaderParsedDevice d{};
    d.key = key->string_value;
    if (!parse_u32(*endpoint, ctx + ".endpoint_index", d.endpoint_index, error)) {
      return false;
    }
    out.devices.push_back(std::move(d));
  }

  out.streams.reserve(streams->array_value.size());
  for (size_t i = 0; i < streams->array_value.size(); ++i) {
    const auto& item = streams->array_value[i];
    const std::string ctx = "streams[" + std::to_string(i) + "]";
    if (item.type != JsonValue::Type::Object) {
      set_error(error, ctx + " must be object");
      return false;
    }
    if (!require_only_fields(item, {"key", "device_key", "intent", "capture_profile"}, error, ctx)) {
      return false;
    }

    const JsonValue* key = find_field(item, "key");
    const JsonValue* device_key = find_field(item, "device_key");
    const JsonValue* intent = find_field(item, "intent");
    const JsonValue* capture_profile = find_field(item, "capture_profile");

    if (!require_type(key, JsonValue::Type::String, ctx + ".key", error) ||
        !require_type(device_key, JsonValue::Type::String, ctx + ".device_key", error) ||
        !require_type(intent, JsonValue::Type::String, ctx + ".intent", error)) {
      return false;
    }

    SyntheticScenarioLoaderParsedStream s{};
    s.key = key->string_value;
    s.device_key = device_key->string_value;
    s.intent = intent->string_value;

    if (!capture_profile) {
      set_error(error, "missing required field: " + ctx + ".capture_profile");
      return false;
    }
    if (!parse_capture_profile(*capture_profile, s.capture_profile, error, ctx + ".capture_profile")) {
      return false;
    }

    out.streams.push_back(std::move(s));
  }

  out.timeline.reserve(timeline->array_value.size());
  for (size_t i = 0; i < timeline->array_value.size(); ++i) {
    const auto& item = timeline->array_value[i];
    const std::string ctx = "timeline[" + std::to_string(i) + "]";
    if (item.type != JsonValue::Type::Object) {
      set_error(error, ctx + " must be object");
      return false;
    }
    if (!require_only_fields(item, {"at_ns", "type", "device_key", "stream_key", "picture"}, error, ctx)) {
      return false;
    }

    const JsonValue* at_ns = find_field(item, "at_ns");
    const JsonValue* type = find_field(item, "type");
    if (!require_type(at_ns, JsonValue::Type::Number, ctx + ".at_ns", error) ||
        !require_type(type, JsonValue::Type::String, ctx + ".type", error)) {
      return false;
    }

    SyntheticScenarioLoaderParsedAction a{};
    if (!parse_u64(*at_ns, ctx + ".at_ns", a.at_ns, error)) {
      return false;
    }
    a.type = type->string_value;

    if (const JsonValue* device_key = find_field(item, "device_key")) {
      if (device_key->type != JsonValue::Type::String) {
        set_error(error, ctx + ".device_key has wrong type");
        return false;
      }
      a.has_device_key = true;
      a.device_key = device_key->string_value;
    }

    if (const JsonValue* stream_key = find_field(item, "stream_key")) {
      if (stream_key->type != JsonValue::Type::String) {
        set_error(error, ctx + ".stream_key has wrong type");
        return false;
      }
      a.has_stream_key = true;
      a.stream_key = stream_key->string_value;
    }

    if (const JsonValue* picture = find_field(item, "picture")) {
      a.has_picture = true;
      if (!parse_picture(*picture, a.picture, error, ctx + ".picture")) {
        return false;
      }
    }

    out.timeline.push_back(std::move(a));
  }

  return true;
}

} // namespace

bool parse_synthetic_scenario_loader_json_text(
    const std::string& text,
    SyntheticScenarioLoaderParsedDocument& out,
    std::string* error) {
  JsonParser parser(text);
  JsonValue root{};
  if (!parser.parse(root, error)) {
    return false;
  }
  return parse_document_object(root, out, error);
}

} // namespace cambang
