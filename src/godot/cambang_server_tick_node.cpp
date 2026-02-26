#include "godot/cambang_server_tick_node.h"

#include "godot/cambang_server.h"

namespace cambang {

void CamBANGServerTickNode::_process(double /*delta*/) {
  if (server_) {
    server_->_on_godot_tick();
  }
}

} // namespace cambang
