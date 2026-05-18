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
    frame.capture_timestamp.domain = CaptureTimestampDomain::CORE_MONOTONIC;
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
  bracket_cmd.payload = CmdProviderFrame{make_capture_frame(CaptureImageRouting::ADDITIONAL_BRACKET, 2000)};
  dispatcher.dispatch(std::move(bracket_cmd));

  auto with_bracket = result_store.get_capture_result(900, 901);
  assert(with_bracket);
  assert(with_bracket->default_image.capture_timestamp_ns == 1000);
  assert(with_bracket->default_image.image_member_index == 0);
  assert(with_bracket->additional_images.size() == 1);
  assert(with_bracket->additional_images[0].role == CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET);
  assert(with_bracket->additional_images[0].capture_timestamp_ns == 2000);
  assert(with_bracket->additional_images[0].image_member_index == 1);
  auto assembly_after_bracket = assembly_registry.find_for_smoke(900, 901);
  assert(assembly_after_bracket && assembly_after_bracket->has_default_image_retained);

  // Bracket-before-base-result must fail deterministically (no default result yet).
  ProviderToCoreCommand bracket_before_base{};
  bracket_before_base.type = ProviderToCoreCommandType::PROVIDER_FRAME;
  auto prebase = make_capture_frame(CaptureImageRouting::ADDITIONAL_BRACKET, 1100);
  prebase.capture_id = 910;
  prebase.device_instance_id = 911;
  bracket_before_base.payload = CmdProviderFrame{prebase};
  dispatcher.dispatch(std::move(bracket_before_base));
  assert(!result_store.get_capture_result(910, 911));
  assert(!assembly_registry.find_for_smoke(910, 911).has_value());

  // Malformed bracket with capture_id==0 is rejected/dropped deterministically.
  ProviderToCoreCommand malformed_no_capture{};
  malformed_no_capture.type = ProviderToCoreCommandType::PROVIDER_FRAME;
  auto no_capture = make_capture_frame(CaptureImageRouting::ADDITIONAL_BRACKET, 1200);
  no_capture.capture_id = 0;
  no_capture.device_instance_id = 912;
  malformed_no_capture.payload = CmdProviderFrame{no_capture};
  dispatcher.dispatch(std::move(malformed_no_capture));
  assert(!result_store.get_capture_result(0, 912));

  // stream_id!=0 bracket route is currently unsupported and rejected.
  ProviderToCoreCommand malformed_with_stream{};
  malformed_with_stream.type = ProviderToCoreCommandType::PROVIDER_FRAME;
  auto with_stream = make_capture_frame(CaptureImageRouting::ADDITIONAL_BRACKET, 1300);
  with_stream.capture_id = 920;
  with_stream.device_instance_id = 913;
  with_stream.stream_id = 77;
  malformed_with_stream.payload = CmdProviderFrame{with_stream};
  dispatcher.dispatch(std::move(malformed_with_stream));
  assert(!result_store.get_capture_result(920, 913));
  assert(!assembly_registry.find_for_smoke(920, 913).has_value());

  std::cout << "PASS core_dispatcher_bracket_routing_smoke\n";
  return 0;
}
