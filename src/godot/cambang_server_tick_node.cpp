#include "godot/cambang_server_tick_node.h"

#include "godot/cambang_server.h"

namespace cambang {

  void CamBANGServerTickNode::_process(double delta) {
    if (CamBANGServer* server = CamBANGServer::get_singleton()) {
      server->_on_godot_tick(delta);
    }
  }

} // namespace cambang