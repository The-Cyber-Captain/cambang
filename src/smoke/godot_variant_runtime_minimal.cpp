#include <gdextension_interface.h>

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "smoke/godot_variant_runtime_minimal.h"

namespace cambang::smoke {

extern bool fake_runtime_installed;
extern bool fake_variant_bindings_initialized;
extern bool fake_string_bindings_initialized;
extern bool fake_dictionary_bindings_initialized;

void fake_int_to_variant(GDExtensionUninitializedVariantPtr dest, GDExtensionTypePtr value_ptr);
void fake_string_to_variant(GDExtensionUninitializedVariantPtr dest, GDExtensionTypePtr value_ptr);
void fake_dictionary_to_variant(
    GDExtensionUninitializedVariantPtr dest,
    GDExtensionTypePtr value_ptr);
void fake_string_constructor_0(
    GDExtensionUninitializedTypePtr base,
    const GDExtensionConstTypePtr* args);
void fake_string_constructor_1(
    GDExtensionUninitializedTypePtr base,
    const GDExtensionConstTypePtr* args);
void fake_string_destructor(GDExtensionTypePtr base);
void fake_dictionary_constructor_0(
    GDExtensionUninitializedTypePtr base,
    const GDExtensionConstTypePtr* args);
void fake_dictionary_constructor_1(
    GDExtensionUninitializedTypePtr base,
    const GDExtensionConstTypePtr* args);
void fake_dictionary_destructor(GDExtensionTypePtr base);

}  // namespace cambang::smoke

namespace godot {
namespace internal {

GDExtensionInterfaceGetProcAddress gdextension_interface_get_proc_address = nullptr;
GDExtensionClassLibraryPtr library = nullptr;
void* token = nullptr;
GDExtensionGodotVersion2 godot_version = {};

GDExtensionInterfaceVariantNewCopy gdextension_interface_variant_new_copy = nullptr;
GDExtensionInterfaceVariantNewNil gdextension_interface_variant_new_nil = nullptr;
GDExtensionInterfaceVariantDestroy gdextension_interface_variant_destroy = nullptr;
GDExtensionInterfaceVariantGetType gdextension_interface_variant_get_type = nullptr;
GDExtensionInterfaceStringNewWithLatin1Chars gdextension_interface_string_new_with_latin1_chars =
    nullptr;
GDExtensionInterfaceDictionaryOperatorIndex gdextension_interface_dictionary_operator_index =
    nullptr;
GDExtensionInterfaceDictionaryOperatorIndexConst
    gdextension_interface_dictionary_operator_index_const = nullptr;

}  // namespace internal

GDExtensionVariantFromTypeConstructorFunc Variant::from_type_constructor[Variant::VARIANT_MAX]{};
GDExtensionTypeFromVariantConstructorFunc Variant::to_type_constructor[Variant::VARIANT_MAX]{};
String::_MethodBindings String::_method_bindings;
Dictionary::_MethodBindings Dictionary::_method_bindings;

void Variant::init_bindings() {
  if (!cambang::smoke::fake_runtime_installed) {
    throw std::runtime_error("fake godot runtime not installed");
  }
  if (!cambang::smoke::fake_variant_bindings_initialized) {
    for (auto& constructor : from_type_constructor) {
      constructor = nullptr;
    }
    for (auto& constructor : to_type_constructor) {
      constructor = nullptr;
    }
    from_type_constructor[INT] = cambang::smoke::fake_int_to_variant;
    from_type_constructor[STRING] = cambang::smoke::fake_string_to_variant;
    from_type_constructor[DICTIONARY] = cambang::smoke::fake_dictionary_to_variant;
    cambang::smoke::fake_variant_bindings_initialized = true;
  }
}

void String::_init_bindings_constructors_destructor() {
  if (!cambang::smoke::fake_runtime_installed) {
    throw std::runtime_error("fake godot runtime not installed");
  }
  if (!cambang::smoke::fake_string_bindings_initialized) {
    _method_bindings = {};
    _method_bindings.constructor_0 = cambang::smoke::fake_string_constructor_0;
    _method_bindings.constructor_1 = cambang::smoke::fake_string_constructor_1;
    _method_bindings.destructor = cambang::smoke::fake_string_destructor;
    cambang::smoke::fake_string_bindings_initialized = true;
  }
}

void String::init_bindings() {
  _init_bindings_constructors_destructor();
}

void Dictionary::_init_bindings_constructors_destructor() {
  if (!cambang::smoke::fake_runtime_installed) {
    throw std::runtime_error("fake godot runtime not installed");
  }
  if (!cambang::smoke::fake_dictionary_bindings_initialized) {
    _method_bindings = {};
    _method_bindings.constructor_0 = cambang::smoke::fake_dictionary_constructor_0;
    _method_bindings.constructor_1 = cambang::smoke::fake_dictionary_constructor_1;
    _method_bindings.destructor = cambang::smoke::fake_dictionary_destructor;
    cambang::smoke::fake_dictionary_bindings_initialized = true;
  }
}

void Dictionary::init_bindings() {
  _init_bindings_constructors_destructor();
}

Variant::Variant() {
  init_bindings();
  internal::gdextension_interface_variant_new_nil(_native_ptr());
}

Variant::Variant(GDExtensionConstVariantPtr native_ptr) {
  init_bindings();
  internal::gdextension_interface_variant_new_copy(_native_ptr(), native_ptr);
}

Variant::Variant(const Variant& other) {
  init_bindings();
  internal::gdextension_interface_variant_new_copy(_native_ptr(), other._native_ptr());
}

Variant::Variant(Variant&& other) {
  init_bindings();
  internal::gdextension_interface_variant_new_copy(_native_ptr(), other._native_ptr());
}

Variant::Variant(int64_t v) {
  init_bindings();
  int64_t encoded = v;
  from_type_constructor[INT](_native_ptr(), &encoded);
}

Variant::Variant(const String& v) {
  init_bindings();
  from_type_constructor[STRING](_native_ptr(), v._native_ptr());
}

Variant::Variant(const Dictionary& v) {
  init_bindings();
  from_type_constructor[DICTIONARY](_native_ptr(), v._native_ptr());
}

Variant::~Variant() {
  internal::gdextension_interface_variant_destroy(_native_ptr());
}

Variant& Variant::operator=(const Variant& other) {
  init_bindings();
  if (this != &other) {
    internal::gdextension_interface_variant_destroy(_native_ptr());
    internal::gdextension_interface_variant_new_copy(_native_ptr(), other._native_ptr());
  }
  return *this;
}

Variant& Variant::operator=(Variant&& other) {
  init_bindings();
  if (this != &other) {
    internal::gdextension_interface_variant_destroy(_native_ptr());
    internal::gdextension_interface_variant_new_copy(_native_ptr(), other._native_ptr());
  }
  return *this;
}

Variant::Type Variant::get_type() const {
  return static_cast<Variant::Type>(internal::gdextension_interface_variant_get_type(_native_ptr()));
}

String::String() {
  init_bindings();
  _method_bindings.constructor_0(&opaque, nullptr);
}

String::String(const String& p_from) {
  init_bindings();
  const GDExtensionConstTypePtr args[] = {&p_from};
  _method_bindings.constructor_1(&opaque, args);
}

String::String(String&& p_other) {
  init_bindings();
  const GDExtensionConstTypePtr args[] = {&p_other};
  _method_bindings.constructor_1(&opaque, args);
}

String::String(const char* p_from) {
  init_bindings();
  internal::gdextension_interface_string_new_with_latin1_chars(_native_ptr(), p_from);
}

String::~String() {
  _method_bindings.destructor(&opaque);
}

Dictionary::Dictionary() {
  init_bindings();
  _method_bindings.constructor_0(&opaque, nullptr);
}

Dictionary::Dictionary(const Dictionary& p_from) {
  init_bindings();
  const GDExtensionConstTypePtr args[] = {&p_from};
  _method_bindings.constructor_1(&opaque, args);
}

Dictionary::Dictionary(Dictionary&& p_other) {
  init_bindings();
  const GDExtensionConstTypePtr args[] = {&p_other};
  _method_bindings.constructor_1(&opaque, args);
}

Dictionary::~Dictionary() {
  _method_bindings.destructor(&opaque);
}

const Variant& Dictionary::operator[](const Variant& p_key) const {
  const Variant* var = (const Variant*)internal::gdextension_interface_dictionary_operator_index_const(
      (GDExtensionTypePtr*)this,
      (GDExtensionVariantPtr)&p_key);
  return *var;
}

Variant& Dictionary::operator[](const Variant& p_key) {
  Variant* var = (Variant*)internal::gdextension_interface_dictionary_operator_index(
      (GDExtensionTypePtr*)this,
      (GDExtensionVariantPtr)&p_key);
  return *var;
}

}  // namespace godot

namespace cambang::smoke {
bool fake_runtime_installed = false;
bool fake_variant_bindings_initialized = false;
bool fake_string_bindings_initialized = false;
bool fake_dictionary_bindings_initialized = false;

namespace {

struct FakeString {
  std::string value;
};

struct FakeVariantState;

struct FakeDictionary {
  std::map<std::string, godot::Variant> entries;
};

struct FakeVariantState {
  GDExtensionVariantType type;
  int64_t int_value;
  void* payload;
};

static_assert(sizeof(FakeVariantState) <= GODOT_CPP_VARIANT_SIZE,
              "fake variant state must fit inside godot::Variant");

FakeVariantState make_nil_state() noexcept {
  return FakeVariantState{
      GDEXTENSION_VARIANT_TYPE_NIL,
      0,
      nullptr,
  };
}

FakeVariantState& state_from_variant_ptr(void* ptr) {
  return *reinterpret_cast<FakeVariantState*>(ptr);
}

const FakeVariantState& state_from_variant_ptr(const void* ptr) {
  return *reinterpret_cast<const FakeVariantState*>(ptr);
}

FakeString*& string_slot(void* ptr) {
  return *reinterpret_cast<FakeString**>(ptr);
}

FakeString* const& string_slot(const void* ptr) {
  return *reinterpret_cast<FakeString* const*>(ptr);
}

FakeDictionary*& dictionary_slot(void* ptr) {
  return *reinterpret_cast<FakeDictionary**>(ptr);
}

FakeDictionary* const& dictionary_slot(const void* ptr) {
  return *reinterpret_cast<FakeDictionary* const*>(ptr);
}

void destroy_constructed_variant_state(FakeVariantState& state) {
  if (state.type == GDEXTENSION_VARIANT_TYPE_STRING) {
    delete static_cast<FakeString*>(state.payload);
  } else if (state.type == GDEXTENSION_VARIANT_TYPE_DICTIONARY) {
    delete static_cast<FakeDictionary*>(state.payload);
  }
  state = make_nil_state();
}

godot::Variant clone_variant(const godot::Variant& src) {
  const FakeVariantState& src_state = state_from_variant_ptr(src._native_ptr());
  switch (src_state.type) {
    case GDEXTENSION_VARIANT_TYPE_NIL:
      return godot::Variant();
    case GDEXTENSION_VARIANT_TYPE_INT:
      return godot::Variant(src_state.int_value);
    case GDEXTENSION_VARIANT_TYPE_STRING:
      if (!src_state.payload) {
        throw std::runtime_error("fake string variant missing payload");
      }
      return godot::Variant(godot::String(static_cast<const FakeString*>(src_state.payload)->value.c_str()));
    case GDEXTENSION_VARIANT_TYPE_DICTIONARY:
      if (!src_state.payload) {
        throw std::runtime_error("fake dictionary variant missing payload");
      }
      return godot::Variant(variant_to_dictionary(src));
    default:
      throw std::runtime_error("unsupported fake variant type");
  }
}

FakeDictionary* require_dictionary_ptr(const godot::Dictionary& dict) {
  FakeDictionary* ptr = dictionary_slot(dict._native_ptr());
  if (!ptr) {
    throw std::runtime_error("expected initialized fake dictionary");
  }
  return ptr;
}

const FakeVariantState* find_entry_state(const godot::Dictionary& dict, std::string_view key) {
  const FakeDictionary* entries = require_dictionary_ptr(dict);
  auto it = entries->entries.find(std::string(key));
  if (it == entries->entries.end()) {
    return nullptr;
  }
  return &state_from_variant_ptr(it->second._native_ptr());
}

void fake_variant_new_nil(GDExtensionUninitializedVariantPtr dest) {
  new (dest) FakeVariantState(make_nil_state());
}

void fake_variant_new_copy(GDExtensionUninitializedVariantPtr dest, GDExtensionConstVariantPtr src) {
  const FakeVariantState& src_state = state_from_variant_ptr(src);
  auto* out = new (dest) FakeVariantState(make_nil_state());
  out->type = src_state.type;
  out->int_value = src_state.int_value;
  if (src_state.type == GDEXTENSION_VARIANT_TYPE_STRING && src_state.payload) {
    out->payload = new FakeString(*static_cast<const FakeString*>(src_state.payload));
  } else if (src_state.type == GDEXTENSION_VARIANT_TYPE_DICTIONARY && src_state.payload) {
    auto dict = std::make_unique<FakeDictionary>();
    const auto& src_entries = static_cast<const FakeDictionary*>(src_state.payload)->entries;
    for (const auto& [key, value] : src_entries) {
      dict->entries.emplace(key, clone_variant(value));
    }
    out->payload = dict.release();
  }
}

void fake_variant_destroy(GDExtensionVariantPtr self) {
  destroy_constructed_variant_state(state_from_variant_ptr(self));
}

GDExtensionVariantType fake_variant_get_type(GDExtensionConstVariantPtr self) {
  return state_from_variant_ptr(self).type;
}

void fake_int_to_variant_impl(
    GDExtensionUninitializedVariantPtr dest,
    GDExtensionTypePtr value_ptr) {
  auto* out = new (dest) FakeVariantState(make_nil_state());
  out->type = GDEXTENSION_VARIANT_TYPE_INT;
  out->int_value = *reinterpret_cast<const int64_t*>(value_ptr);
}

void fake_string_to_variant_impl(
    GDExtensionUninitializedVariantPtr dest,
    GDExtensionTypePtr value_ptr) {
  auto* out = new (dest) FakeVariantState(make_nil_state());
  out->type = GDEXTENSION_VARIANT_TYPE_STRING;
  const FakeString* src_string = string_slot(value_ptr);
  out->payload = new FakeString(src_string ? *src_string : FakeString{});
}

void fake_dictionary_to_variant_impl(
    GDExtensionUninitializedVariantPtr dest,
    GDExtensionTypePtr value_ptr) {
  auto* out = new (dest) FakeVariantState(make_nil_state());
  out->type = GDEXTENSION_VARIANT_TYPE_DICTIONARY;
  auto dict = std::make_unique<FakeDictionary>();
  const FakeDictionary* src_dict = dictionary_slot(value_ptr);
  if (src_dict) {
    for (const auto& [key, value] : src_dict->entries) {
      dict->entries.emplace(key, clone_variant(value));
    }
  }
  out->payload = dict.release();
}

void fake_string_constructor_0_impl(
    GDExtensionUninitializedTypePtr base,
    const GDExtensionConstTypePtr* args) {
  (void)args;
  string_slot(base) = new FakeString();
}

void fake_string_constructor_1_impl(
    GDExtensionUninitializedTypePtr base,
    const GDExtensionConstTypePtr* args) {
  const auto* src = reinterpret_cast<const godot::String*>(args[0]);
  const FakeString* src_string = string_slot(src->_native_ptr());
  string_slot(base) = new FakeString(src_string ? *src_string : FakeString{});
}

void fake_string_destructor_impl(GDExtensionTypePtr base) {
  delete string_slot(base);
  string_slot(base) = nullptr;
}

void fake_dictionary_constructor_0_impl(
    GDExtensionUninitializedTypePtr base,
    const GDExtensionConstTypePtr* args) {
  (void)args;
  dictionary_slot(base) = new FakeDictionary();
}

void fake_dictionary_constructor_1_impl(
    GDExtensionUninitializedTypePtr base,
    const GDExtensionConstTypePtr* args) {
  auto* src = reinterpret_cast<const godot::Dictionary*>(args[0]);
  auto dict = std::make_unique<FakeDictionary>();
  const FakeDictionary* src_dict = dictionary_slot(src->_native_ptr());
  if (src_dict) {
    for (const auto& [key, value] : src_dict->entries) {
      dict->entries.emplace(key, clone_variant(value));
    }
  }
  dictionary_slot(base) = dict.release();
}

void fake_dictionary_destructor_impl(GDExtensionTypePtr base) {
  delete dictionary_slot(base);
  dictionary_slot(base) = nullptr;
}

void fake_string_new_with_latin1_chars(
    GDExtensionUninitializedStringPtr dest,
    const char* contents) {
  string_slot(dest) = new FakeString{contents ? contents : ""};
}

std::string key_string_from_variant(const godot::Variant& key) {
  const FakeVariantState& state = state_from_variant_ptr(key._native_ptr());
  if (state.type != GDEXTENSION_VARIANT_TYPE_STRING || !state.payload) {
    throw std::runtime_error("converter smoke expected string dictionary keys");
  }
  return static_cast<const FakeString*>(state.payload)->value;
}

GDExtensionVariantPtr fake_dictionary_operator_index(
    GDExtensionTypePtr self,
    GDExtensionConstVariantPtr key) {
  auto* dict = dictionary_slot(self);
  if (!dict) {
    throw std::runtime_error("dictionary operator on uninitialized dictionary");
  }
  const std::string key_text = key_string_from_variant(
      *reinterpret_cast<const godot::Variant*>(key));
  return dict->entries[key_text]._native_ptr();
}

GDExtensionVariantPtr fake_dictionary_operator_index_const(
    GDExtensionConstTypePtr self,
    GDExtensionConstVariantPtr key) {
  auto* dict = dictionary_slot(const_cast<void*>(self));
  if (!dict) {
    throw std::runtime_error("dictionary operator on uninitialized dictionary");
  }
  const std::string key_text = key_string_from_variant(
      *reinterpret_cast<const godot::Variant*>(key));
  auto it = dict->entries.find(key_text);
  if (it == dict->entries.end()) {
    static godot::Variant nil_value;
    return nil_value._native_ptr();
  }
  return it->second._native_ptr();
}

}  // namespace

void fake_int_to_variant(GDExtensionUninitializedVariantPtr dest, GDExtensionTypePtr value_ptr) {
  fake_int_to_variant_impl(dest, value_ptr);
}

void fake_string_to_variant(
    GDExtensionUninitializedVariantPtr dest,
    GDExtensionTypePtr value_ptr) {
  fake_string_to_variant_impl(dest, value_ptr);
}

void fake_dictionary_to_variant(
    GDExtensionUninitializedVariantPtr dest,
    GDExtensionTypePtr value_ptr) {
  fake_dictionary_to_variant_impl(dest, value_ptr);
}

void fake_string_constructor_0(
    GDExtensionUninitializedTypePtr base,
    const GDExtensionConstTypePtr* args) {
  fake_string_constructor_0_impl(base, args);
}

void fake_string_constructor_1(
    GDExtensionUninitializedTypePtr base,
    const GDExtensionConstTypePtr* args) {
  fake_string_constructor_1_impl(base, args);
}

void fake_string_destructor(GDExtensionTypePtr base) {
  fake_string_destructor_impl(base);
}

void fake_dictionary_constructor_0(
    GDExtensionUninitializedTypePtr base,
    const GDExtensionConstTypePtr* args) {
  fake_dictionary_constructor_0_impl(base, args);
}

void fake_dictionary_constructor_1(
    GDExtensionUninitializedTypePtr base,
    const GDExtensionConstTypePtr* args) {
  fake_dictionary_constructor_1_impl(base, args);
}

void fake_dictionary_destructor(GDExtensionTypePtr base) {
  fake_dictionary_destructor_impl(base);
}

void install_fake_godot_runtime() {
  godot::internal::gdextension_interface_variant_new_nil = fake_variant_new_nil;
  godot::internal::gdextension_interface_variant_new_copy = fake_variant_new_copy;
  godot::internal::gdextension_interface_variant_destroy = fake_variant_destroy;
  godot::internal::gdextension_interface_variant_get_type = fake_variant_get_type;
  godot::internal::gdextension_interface_string_new_with_latin1_chars =
      fake_string_new_with_latin1_chars;
  godot::internal::gdextension_interface_dictionary_operator_index =
      fake_dictionary_operator_index;
  godot::internal::gdextension_interface_dictionary_operator_index_const =
      fake_dictionary_operator_index_const;
  fake_runtime_installed = true;
  fake_variant_bindings_initialized = false;
  fake_string_bindings_initialized = false;
  fake_dictionary_bindings_initialized = false;
}

bool dictionary_has_key(const godot::Dictionary& dict, std::string_view key) {
  return find_entry_state(dict, key) != nullptr;
}

godot::Variant dictionary_find_value(const godot::Dictionary& dict, std::string_view key) {
  const FakeVariantState* state = find_entry_state(dict, key);
  if (!state) {
    return godot::Variant();
  }
  const FakeDictionary* entries = require_dictionary_ptr(dict);
  auto it = entries->entries.find(std::string(key));
  return clone_variant(it->second);
}

godot::Dictionary variant_to_dictionary(const godot::Variant& value) {
  const FakeVariantState& state = state_from_variant_ptr(value._native_ptr());
  if (state.type != GDEXTENSION_VARIANT_TYPE_DICTIONARY || !state.payload) {
    throw std::runtime_error("expected dictionary variant");
  }
  godot::Dictionary out;
  const auto& src_entries = static_cast<const FakeDictionary*>(state.payload)->entries;
  for (const auto& [key, entry] : src_entries) {
    out[godot::String(key.c_str())] = clone_variant(entry);
  }
  return out;
}

GDExtensionVariantType variant_type(const godot::Variant& value) {
  return state_from_variant_ptr(value._native_ptr()).type;
}

int64_t variant_int(const godot::Variant& value) {
  const FakeVariantState& state = state_from_variant_ptr(value._native_ptr());
  if (state.type != GDEXTENSION_VARIANT_TYPE_INT) {
    throw std::runtime_error("expected int variant");
  }
  return state.int_value;
}

std::string variant_string(const godot::Variant& value) {
  const FakeVariantState& state = state_from_variant_ptr(value._native_ptr());
  if (state.type != GDEXTENSION_VARIANT_TYPE_STRING || !state.payload) {
    throw std::runtime_error("expected string variant");
  }
  return static_cast<const FakeString*>(state.payload)->value;
}

}  // namespace cambang::smoke
