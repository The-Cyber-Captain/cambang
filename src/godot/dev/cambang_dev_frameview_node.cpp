// src/godot/dev/cambang_dev_frameview_node.cpp

#include "godot/dev/cambang_dev_frameview_node.h"

#include <cstring>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/property_info.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include "godot/dev/cambang_dev_node.h"

using godot::UtilityFunctions;

namespace cambang {

void CamBANGDevFrameViewNode::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("set_dev_node_path", "path"),
                             &CamBANGDevFrameViewNode::set_dev_node_path);
  godot::ClassDB::bind_method(godot::D_METHOD("get_dev_node_path"),
                             &CamBANGDevFrameViewNode::get_dev_node_path);
  ADD_PROPERTY(godot::PropertyInfo(godot::Variant::NODE_PATH, "dev_node_path"),
               "set_dev_node_path", "get_dev_node_path");
}

void CamBANGDevFrameViewNode::_ready() {
  if (godot::Engine::get_singleton()->is_editor_hint()) {
    // No runtime in-editor; keep node inert.
    set_process(false);
    return;
  }
  // Configure as a visual node by default.
  set_stretch_mode(godot::TextureRect::STRETCH_SCALE);
  set_expand_mode(godot::TextureRect::EXPAND_IGNORE_SIZE);
  set_process(true);
  resolve_deps_();
}

  void CamBANGDevFrameViewNode::_process(double delta) {
  if (!mailbox_) {
    resolve_deps_();
    if (!mailbox_) {
      return;
    }
  }

  // Dev-only visibility for Windows MF: show whether frames are arriving, accepted, or dropped.
  stats_accum_ += delta;
  if (stats_accum_ >= 1.0) {
    stats_accum_ = 0.0;
    const auto s = mailbox_->get_stats();
    UtilityFunctions::print(
        "[CamBANG] mailbox stats:",
        " received=", (long long)s.frames_received,
        " accepted_rgba=", (long long)s.accepted_rgba,
        " accepted_bgra_swizzled=", (long long)s.accepted_bgra_swizzled,
        " published=", (long long)s.frames_published,
        " dropped_unsupported=", (long long)s.frames_dropped_unsupported,
        " dropped_invalid=", (long long)s.frames_dropped_invalid
    );
  }

  try_update_();
}

void CamBANGDevFrameViewNode::resolve_deps_() {
  dev_node_ = nullptr;
  mailbox_ = nullptr;

  if (dev_node_path_.is_empty()) {
    return;
  }

  auto* n = get_node_or_null(dev_node_path_);
  if (!n) {
    return;
  }

  dev_node_ = godot::Object::cast_to<CamBANGDevNode>(n);
  if (!dev_node_) {
    UtilityFunctions::printerr("[CamBANGDevFrameViewNode] dev_node_path is not a CamBANGDevNode.");
    return;
  }

  mailbox_ = dev_node_->get_latest_frame_mailbox();
}

static godot::PackedByteArray to_pba(const std::vector<uint8_t>& bytes) {
  godot::PackedByteArray out;
  const int64_t n = static_cast<int64_t>(bytes.size());
  out.resize(n);
  if (n > 0) {
    std::memcpy(out.ptrw(), bytes.data(), static_cast<size_t>(n));
  }
  return out;
}

void CamBANGDevFrameViewNode::try_update_() {
  RgbaFrame fc;
  if (!mailbox_->try_copy_if_new(last_seq_, fc)) {
    return;
  }

  // Mailbox publishes *only* tightly packed RGBA8. Any YUV/RAW formats should
  // be handled by a different sink (later) and are intentionally not converted
  // here to avoid baking in CPU overhead.
  if (fc.width == 0 || fc.height == 0 || fc.pixels.empty()) {
    return;
  }

  const size_t expected = static_cast<size_t>(fc.width) * static_cast<size_t>(fc.height) * 4u;
  if (fc.pixels.size() != expected) {
    // Godot Image::create_from_data requires tightly packed bytes.
    return;
  }

  // Prefer the static factory: it returns a Ref<Image> that is either valid or empty.
  godot::Ref<godot::Image> img = godot::Image::create_from_data(
      static_cast<int>(fc.width),
      static_cast<int>(fc.height),
      false,
      godot::Image::FORMAT_RGBA8,
      to_pba(fc.pixels));
  if (img.is_null() || img->is_empty()) {
    return;
  }

  if (texture_.is_null()) {
    texture_ = godot::ImageTexture::create_from_image(img);
    if (texture_.is_null()) {
      return;
    }
    set_texture(texture_);
  } else {
    texture_->update(img);
  }

  // Only advance last_seq_ after a successful upload path.
  last_seq_ = fc.seq;
}

} // namespace cambang