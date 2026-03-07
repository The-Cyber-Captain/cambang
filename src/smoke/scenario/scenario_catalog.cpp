#include "smoke/scenario/scenario_catalog.h"

namespace cambang {

bool load_scenario(const std::string& name, Scenario& out) {
  if (name == "stream_lifecycle") {
    Scenario s("stream_lifecycle");
    s.at(0).open_device("cam0").expect(1, 0, true);
    s.at(1).create_stream("cam0").expect(1, 0, true);
    s.at(2).start_stream("cam0").expect(1, 1, false);
    s.at(3).emit_frame("cam0").expect(1, 1, false);
    s.at(4).stop_stream("cam0").expect(1, 0, false);
    out = std::move(s);
    return true;
  }

  if (name == "topology_lifecycle") {
    Scenario s("topology_lifecycle");
    s.at(0).open_device("cam0").expect(1, 0, true);
    s.at(1).create_stream("cam0").expect(1, 0, true);
    s.at(2).destroy_stream("cam0").expect(1, 0, true);
    s.at(3).close_device("cam0").expect(0, 0, true);
    out = std::move(s);
    return true;
  }

  if (name == "stop_fact_error") {
    Scenario s("stop_fact_error", ScenarioProviderMask::Stub);
    s.at(0).open_device("cam0").expect(1, 0, true);
    s.at(1).create_stream("cam0").expect(1, 0, true);
    s.at(2).start_stream("cam0").expect(1, 1, false);
    s.at(3).inject_stop_error("cam0").expect(1, 0, false);
    out = std::move(s);
    return true;
  }

  return false;
}

} // namespace cambang
