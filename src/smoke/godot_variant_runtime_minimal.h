#pragma once

#include <gdextension_interface.h>

#include <cstdint>
#include <string>
#include <string_view>

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace cambang::smoke {

void install_fake_godot_runtime();

bool dictionary_has_key(const godot::Dictionary& dict, std::string_view key);
godot::Variant dictionary_find_value(const godot::Dictionary& dict, std::string_view key);
godot::Dictionary variant_to_dictionary(const godot::Variant& value);
GDExtensionVariantType variant_type(const godot::Variant& value);
int64_t variant_int(const godot::Variant& value);
std::string variant_string(const godot::Variant& value);

}  // namespace cambang::smoke
