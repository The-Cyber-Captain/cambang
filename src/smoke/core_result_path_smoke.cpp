#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "core/core_result_store.h"

using namespace cambang;

int main() {
  CoreResultStore store;

  std::vector<uint8_t> px(2 * 2 * 4, 7);
  FrameView stream_frame{};
  stream_frame.device_instance_id = 10;
  stream_frame.stream_id = 20;
  stream_frame.width = 2;
  stream_frame.height = 2;
  stream_frame.format_fourcc = FOURCC_RGBA;
  stream_frame.data = px.data();
  stream_frame.size_bytes = px.size();
  stream_frame.stride_bytes = 0;
  store.retain_frame(stream_frame, StreamIntent::VIEWFINDER, 1234);

  auto stream_result = store.get_latest_stream_result(20);
  assert(stream_result);
  assert(stream_result->stream_id == 20);
  assert(stream_result->payload.width == 2);
  assert(stream_result->payload_kind == ResultPayloadKind::CPU_PACKED);
  assert(stream_result->retained_access_truth.display_view == ResultCapability::CHEAP);
  assert(stream_result->retained_access_truth.to_image == ResultCapability::CHEAP);
  assert(stream_result->retained_access_truth.encoded_bytes == ResultCapability::UNSUPPORTED);
  assert(stream_result->access_posture.posture_id != 0);
  assert(stream_result->access_posture.payload_kind == ResultPayloadKind::CPU_PACKED);
  assert(stream_result->access_posture.has_retained_cpu_payload);
  assert(!stream_result->access_posture.has_retained_gpu_backing);

  FrameView gpu_only_stream_frame = stream_frame;
  gpu_only_stream_frame.stream_id = 21;
  gpu_only_stream_frame.primary_backing_kind = ProducerBackingKind::GPU;
  gpu_only_stream_frame.primary_backing_artifact = std::make_shared<int>(42);
  gpu_only_stream_frame.retain_cpu_sidecar = false;
  store.retain_frame(gpu_only_stream_frame, StreamIntent::VIEWFINDER, 1236);

  auto gpu_only_stream_result = store.get_latest_stream_result(21);
  assert(gpu_only_stream_result);
  assert(gpu_only_stream_result->payload_kind == ResultPayloadKind::GPU_SURFACE);
  assert(gpu_only_stream_result->retained_gpu_backing);
  assert(gpu_only_stream_result->retained_access_truth.display_view == ResultCapability::READY);
  assert(gpu_only_stream_result->retained_access_truth.to_image == ResultCapability::UNSUPPORTED);
  assert(gpu_only_stream_result->retained_access_truth.encoded_bytes == ResultCapability::UNSUPPORTED);

  FrameView gpu_materializable_stream_frame = gpu_only_stream_frame;
  gpu_materializable_stream_frame.stream_id = 23;
  gpu_materializable_stream_frame.retained_gpu_backing_descriptor.valid = true;
  gpu_materializable_stream_frame.retained_gpu_backing_descriptor.materialization_available = true;
  store.retain_frame(gpu_materializable_stream_frame, StreamIntent::VIEWFINDER, 1238);

  auto gpu_materializable_stream_result = store.get_latest_stream_result(23);
  assert(gpu_materializable_stream_result);
  assert(gpu_materializable_stream_result->payload_kind == ResultPayloadKind::GPU_SURFACE);
  assert(gpu_materializable_stream_result->retained_gpu_backing);
  assert(gpu_materializable_stream_result->retained_access_truth.display_view == ResultCapability::READY);
  assert(gpu_materializable_stream_result->retained_access_truth.to_image == ResultCapability::EXPENSIVE);
  assert(gpu_materializable_stream_result->retained_access_truth.encoded_bytes == ResultCapability::UNSUPPORTED);
  assert(gpu_materializable_stream_result->access_posture.payload_kind == ResultPayloadKind::GPU_SURFACE);
  assert(!gpu_materializable_stream_result->access_posture.has_retained_cpu_payload);
  assert(gpu_materializable_stream_result->access_posture.has_retained_gpu_backing);
  assert(gpu_materializable_stream_result->access_posture.gpu_materialization_available);

  FrameView gpu_stream_frame = stream_frame;
  gpu_stream_frame.stream_id = 22;
  gpu_stream_frame.primary_backing_kind = ProducerBackingKind::GPU;
  gpu_stream_frame.primary_backing_artifact = std::make_shared<int>(43);
  gpu_stream_frame.retain_cpu_sidecar = true;
  store.retain_frame(gpu_stream_frame, StreamIntent::VIEWFINDER, 1237);

  auto gpu_stream_result = store.get_latest_stream_result(22);
  assert(gpu_stream_result);
  assert(gpu_stream_result->payload_kind == ResultPayloadKind::GPU_SURFACE);
  assert(gpu_stream_result->retained_gpu_backing);
  assert(gpu_stream_result->retained_access_truth.display_view == ResultCapability::READY);
  assert(gpu_stream_result->retained_access_truth.to_image == ResultCapability::CHEAP);
  assert(gpu_stream_result->retained_access_truth.encoded_bytes == ResultCapability::UNSUPPORTED);

  store.mark_stream_display_demand(20, 1'000'000'000ull);
  assert(store.is_stream_display_demand_active(20, 1'150'000'000ull));
  assert(!store.is_stream_display_demand_active(20, 1'260'000'001ull));

  // Demand marks for unknown streams are ignored/evicted to bound the map.
  store.mark_stream_display_demand(999, 2'000'000'000ull);
  assert(!store.is_stream_display_demand_active(999, 2'000'000'000ull));

  store.remove_stream_result(20);
  assert(!store.get_latest_stream_result(20));
  assert(!store.is_stream_display_demand_active(20, 1'150'000'000ull));

  store.retain_frame(stream_frame, StreamIntent::VIEWFINDER, 1235);
  assert(store.get_latest_stream_result(20));
  store.mark_stream_display_demand(20, 3'000'000'000ull);
  assert(store.is_stream_display_demand_active(20, 3'010'000'000ull));

  FrameView capture_a = stream_frame;
  capture_a.stream_id = 0;
  capture_a.capture_id = 77;
  capture_a.device_instance_id = 100;
  store.retain_frame(capture_a, std::nullopt, 2000);

  FrameView capture_b = stream_frame;
  capture_b.stream_id = 0;
  capture_b.capture_id = 77;
  capture_b.device_instance_id = 101;
  store.retain_frame(capture_b, std::nullopt, 2001);

  auto capture_result = store.get_capture_result(77, 100);
  assert(capture_result);
  assert(capture_result->capture_id == 77);
  assert(capture_result->device_instance_id == 100);
  assert(capture_result->default_image.image_member_index == 0);
  assert(capture_result->default_image.role == CoreCaptureResultData::ImageMemberRole::DEFAULT_METERED);
  assert(capture_result->default_image.payload.width == 2);
  assert(capture_result->default_image.retained_access_truth.display_view == ResultCapability::CHEAP);
  assert(capture_result->default_image.retained_access_truth.to_image == ResultCapability::CHEAP);
  assert(capture_result->default_image.retained_access_truth.encoded_bytes == ResultCapability::UNSUPPORTED);
  assert(capture_result->default_image.access_posture.posture_id != 0);
  assert(capture_result->default_image.access_posture.device_instance_id == 100);
  assert(capture_result->default_image.access_posture.has_retained_cpu_payload);
  assert(capture_result->additional_images.empty());

  FrameView gpu_capture = capture_a;
  gpu_capture.capture_id = 78;
  gpu_capture.data = nullptr;
  gpu_capture.size_bytes = 0;
  gpu_capture.primary_backing_kind = ProducerBackingKind::GPU;
  gpu_capture.primary_backing_artifact = std::make_shared<int>(44);
  gpu_capture.retain_cpu_sidecar = false;
  gpu_capture.retained_gpu_backing_descriptor.valid = true;
  gpu_capture.retained_gpu_backing_descriptor.materialization_available = true;
  assert(store.retain_frame(gpu_capture, std::nullopt, 2007));
  auto gpu_capture_result = store.get_capture_result(78, 100);
  assert(gpu_capture_result);
  assert(gpu_capture_result->payload_kind == ResultPayloadKind::GPU_SURFACE);
  assert(gpu_capture_result->default_image.payload_kind == ResultPayloadKind::GPU_SURFACE);
  assert(gpu_capture_result->default_image.payload.empty());
  assert(gpu_capture_result->default_image.retained_gpu_backing);
  assert(gpu_capture_result->default_image.retained_access_truth.to_image == ResultCapability::EXPENSIVE);
  assert(gpu_capture_result->default_image.retained_access_truth.encoded_bytes == ResultCapability::UNSUPPORTED);
  assert(gpu_capture_result->default_image.access_posture.posture_id != 0);
  assert(gpu_capture_result->default_image.access_posture.has_retained_gpu_backing);
  assert(gpu_capture_result->default_image.access_posture.gpu_materialization_available);

  const uint64_t capture_id_before = capture_result->capture_id;
  const uint64_t device_id_before = capture_result->device_instance_id;
  const uint32_t width_before = capture_result->image_width;
  const uint32_t height_before = capture_result->image_height;
  const uint32_t format_before = capture_result->image_format_fourcc;
  const auto payload_kind_before = capture_result->payload_kind;
  const uint64_t default_ts_before = capture_result->default_image.capture_timestamp_ns;
  const size_t default_bytes_before = capture_result->default_image.payload.size_bytes();

  CoreCaptureResultData::ImageMemberData bracket{};
  bracket.image_member_index = 1;
  bracket.role = CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET;
  bracket.capture_timestamp_ns = 2002;
  bracket.payload = capture_result->default_image.payload;
  assert(store.append_additional_capture_image(77, 100, bracket));

  auto capture_result_with_bracket = store.get_capture_result(77, 100);
  assert(capture_result_with_bracket);
  assert(capture_result_with_bracket->default_image.capture_timestamp_ns == default_ts_before);
  assert(capture_result_with_bracket->default_image.payload.size_bytes() == default_bytes_before);
  assert(capture_result_with_bracket->additional_images.size() == 1);
  assert(capture_result_with_bracket->additional_images[0].role == CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET);
  assert(capture_result_with_bracket->additional_images[0].image_member_index == 1);
  assert(capture_result_with_bracket->additional_images[0].retained_access_truth.display_view == ResultCapability::CHEAP);
  assert(capture_result_with_bracket->additional_images[0].retained_access_truth.to_image == ResultCapability::CHEAP);
  assert(capture_result_with_bracket->additional_images[0].retained_access_truth.encoded_bytes == ResultCapability::UNSUPPORTED);
  assert(capture_result_with_bracket->additional_images[0].access_posture.posture_id != 0);
  assert(capture_result_with_bracket->additional_images[0].access_posture.device_instance_id == 100);
  assert(capture_result_with_bracket->additional_images[0].access_posture.has_retained_cpu_payload);
  assert(capture_result_with_bracket->capture_id == capture_id_before);
  assert(capture_result_with_bracket->device_instance_id == device_id_before);
  assert(capture_result_with_bracket->image_width == width_before);
  assert(capture_result_with_bracket->image_height == height_before);
  assert(capture_result_with_bracket->image_format_fourcc == format_before);
  assert(capture_result_with_bracket->payload_kind == payload_kind_before);

  CoreCaptureResultData::ImageMemberData bad_role{};
  bad_role.role = CoreCaptureResultData::ImageMemberRole::DEFAULT_METERED;
  bad_role.capture_timestamp_ns = 2003;
  bad_role.payload = capture_result_with_bracket->default_image.payload;
  assert(!store.append_additional_capture_image(77, 100, bad_role));

  CoreCaptureResultData::ImageMemberData bad_payload{};
  bad_payload.image_member_index = 2;
  bad_payload.role = CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET;
  bad_payload.capture_timestamp_ns = 2004;
  bad_payload.payload = CoreResultPayloadCpuPacked{};
  assert(!store.append_additional_capture_image(77, 100, bad_payload));
  CoreCaptureResultData::ImageMemberData out_of_order{};
  out_of_order.image_member_index = 3;
  out_of_order.role = CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET;
  out_of_order.capture_timestamp_ns = 2005;
  out_of_order.payload = capture_result_with_bracket->default_image.payload;
  assert(!store.append_additional_capture_image(77, 100, out_of_order));
  CoreCaptureResultData::ImageMemberData duplicate_index{};
  duplicate_index.image_member_index = 1;
  duplicate_index.role = CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET;
  duplicate_index.capture_timestamp_ns = 2006;
  duplicate_index.payload = capture_result_with_bracket->default_image.payload;
  assert(!store.append_additional_capture_image(77, 100, duplicate_index));
  assert(!store.append_additional_capture_image(999, 100, bracket));

  auto capture_set = store.get_capture_result_set(77);
  assert(capture_set.size() == 2);

  store.clear();
  assert(!store.get_latest_stream_result(20));
  assert(!store.is_stream_display_demand_active(20, 3'010'000'000ull));

  // mailbox/result independence smoke proxy: result path exists without a sink.
  CoreResultStore no_mailbox_store;
  no_mailbox_store.retain_frame(stream_frame, StreamIntent::PREVIEW, 111);
  assert(no_mailbox_store.get_latest_stream_result(20));

  std::cout << "PASS core_result_path_smoke\n";
  return 0;
}
