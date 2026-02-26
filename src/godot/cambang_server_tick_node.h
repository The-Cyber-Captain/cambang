#pragma once

#include <godot_cpp/classes/node.hpp>

namespace cambang {

class CamBANGServer;

// Internal helper: a node that lives under /root to receive _process callbacks.
// This keeps CamBANGServer as an Engine singleton Object (not a scene node)
// while still allowing main-thread polling + signal emission.
class CamBANGServerTickNode final : public godot::Node {
  GDCLASS(CamBANGServerTickNode, godot::Node)

public:
  CamBANGServerTickNode() = default;
  ~CamBANGServerTickNode() override = default;

  void set_server(CamBANGServer* server) noexcept { server_ = server; }

  void _process(double delta) override;

protected:
  static void _bind_methods() {}

private:
  CamBANGServer* server_ = nullptr; // not owned
};

} // namespace cambang
