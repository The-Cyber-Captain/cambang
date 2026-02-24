// src/godot/dev/cambang_dev_frameview_node.h
#pragma once

#include <cstdint>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/texture_rect.hpp>

#include "core/latest_frame_mailbox.h"

namespace cambang {

class CamBANGDevNode;

// Dev-only visual node: polls LatestFrameMailbox and displays as a TextureRect.
//
// This node is intended for tests/cambang_gde only.
class CamBANGDevFrameViewNode final : public godot::TextureRect {
  GDCLASS(CamBANGDevFrameViewNode, godot::TextureRect)

public:
  CamBANGDevFrameViewNode() = default;
  ~CamBANGDevFrameViewNode() override = default;

  void _ready() override;
  void _process(double delta) override;

  void set_dev_node_path(const godot::NodePath& path) { dev_node_path_ = path; }
  godot::NodePath get_dev_node_path() const { return dev_node_path_; }

protected:
  static void _bind_methods();

private:
  godot::NodePath dev_node_path_;
  CamBANGDevNode* dev_node_ = nullptr;
  const LatestFrameMailbox* mailbox_ = nullptr;
  uint64_t last_seq_ = 0;

  godot::Ref<godot::ImageTexture> texture_;

  void resolve_deps_();
  void try_update_();
};

} // namespace cambang
