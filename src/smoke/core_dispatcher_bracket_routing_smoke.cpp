#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#include "core/core_capture_assembly_registry.h"
#include "core/core_dispatcher.h"
#include "core/core_result_store.h"

using namespace cambang;

int main() {
  CoreStreamRegistry streams;
  CoreResultStore result_store;
  CoreCaptureAssemblyRegistry assembly_registry;
  uint64_t gen = 1;
  CoreDispatcher dispatcher(&streams, nullptr, nullptr, nullptr, &gen, []() { return 0; });
  dispatcher.set_result_store(&result_store);
  dispatcher.set_capture_assembly_registry(&assembly_registry);

  std::vector<uint8_t> px(2 * 2 * 4, 9);
  auto make_capture_frame = [&](CaptureImageRouting routing, uint64_t ts) {
    FrameView frame{};
    frame.capture_id = 900;
    frame.device_instance_id = 901;
    frame.capture_image_routing = routing;
    frame.width = 2;
    frame.height = 2;
    frame.format_fourcc = FOURCC_RGBA;
    frame.data = px.data();
    frame.size_bytes = px.size();
    frame.capture_timestamp.value = ts;
    frame.capture_timestamp.tick_ns = 1;
    return frame;
  };

  ProviderToCoreCommand default_cmd{};
  default_cmd.type = ProviderToCoreCommandType::PROVIDER_FRAME;
  default_cmd.payload = CmdProviderFrame{make_capture_frame(CaptureImageRouting::DEFAULT_METERED, 1000)};
  dispatcher.dispatch(std::move(default_cmd));

  auto base = result_store.get_capture_result(900, 901);
  assert(base);
  assert(base->default_image.role == CoreCaptureResultData::ImageMemberRole::DEFAULT_METERED);
  assert(base->additional_images.empty());
  auto assembly = assembly_registry.find_for_smoke(900, 901);
  assert(assembly && assembly->has_default_image_retained);

  ProviderToCoreCommand bracket_cmd{};
  bracket_cmd.type = ProviderToCoreCommandType::PROVIDER_FRAME;
  bracket_cmd.payload = CmdProviderFrame{make_capture_frame(CaptureImageRouting::ADDITIONAL_BRACKET, 1001)};
  dispatcher.dispatch(std::move(bracket_cmd));

  auto with_bracket = result_store.get_capture_result(900, 901);
  assert(with_bracket);
  assert(with_bracket->default_image.capture_timestamp_ns == 1000);
  assert(with_bracket->additional_images.size() == 1);
  assert(with_bracket->additional_images[0].role == CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET);
  assert(with_bracket->additional_images[0].capture_timestamp_ns == 1001);
  assert(with_bracket->additional_images[0].image_member_index == 1);

  std::cout << "PASS core_dispatcher_bracket_routing_smoke\n";
  return 0;
}
