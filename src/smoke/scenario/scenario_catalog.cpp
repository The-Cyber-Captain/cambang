#include "smoke/scenario/scenario_catalog.h"

namespace cambang {

bool load_scenario(const std::string& name, Scenario& out) {
  if (name != "stream_lifecycle") {
    return false;
  }

  Scenario s("stream_lifecycle");
  s.at(0).open_device("cam0").expect(1, 0);
  s.at(1).create_stream("cam0").expect(1, 0);
  s.at(2).start_stream("cam0").expect(1, 1);
  s.at(3).emit_frame("cam0").expect(1, 1);
  s.at(4).stop_stream("cam0").expect(1, 0);

  out = std::move(s);
  return true;
}

} // namespace cambang
