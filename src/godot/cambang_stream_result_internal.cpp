#include "godot/cambang_stream_result_internal.h"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "godot/cambang_server.h"
#include "godot/display_demand_dispatcher.h"
#include "godot/render_resource_release_service.h"

namespace cambang {

namespace {

bool display_demand_trace_enabled() {
  const char* v = std::getenv("CAMBANG_DEV_DISPLAY_DEMAND_TRACE");
  return v && v[0] != '\0' && v[0] != '0';
}

struct CpuDisplayWrapperBorrow final {
  uint64_t stream_id = 0;
  uint64_t wrapper_instance_id = 0;
};

std::mutex g_live_cpu_display_wrapper_borrow_mutex;
std::map<uint64_t, CpuDisplayWrapperBorrow> g_live_cpu_display_wrapper_borrows;
std::map<uint64_t, uint32_t> g_live_cpu_display_wrapper_borrow_counts;
uint64_t g_next_live_cpu_display_wrapper_borrow_id = 1;

DisplayDemandDispatcher g_display_demand_dispatcher;

uint64_t retain_display_demand_target(void* target, uint64_t stream_id) noexcept {
  return static_cast<CamBANGServer*>(target)->retain_stream_display_demand_from_owner(stream_id);
}

void release_display_demand_target(void* target, uint64_t lease_token) noexcept {
  static_cast<CamBANGServer*>(target)->release_stream_display_demand_from_owner(lease_token);
}

void release_display_demand_from_owner(uint64_t lease_token) noexcept {
  g_display_demand_dispatcher.release(lease_token);
}

uint64_t retain_display_demand_for_owner(uint64_t stream_id) noexcept {
  return g_display_demand_dispatcher.retain(stream_id);
}
}

godot::RID SharedLiveCpuTextureRidState::snapshot_rid() const {
  std::lock_guard<std::mutex> lock(mutex);
  return texture_rid;
}

bool SharedLiveCpuTextureRidState::replace_rid(
    const godot::RID& texture_rid_in,
    RenderResourceReleaseReservation& reservation) noexcept {
  godot::RID prior;
  RenderResourceReleaseReservation prior_reservation;
  try {
    std::lock_guard<std::mutex> lock(mutex);
    prior = texture_rid;
    prior_reservation = std::move(release_reservation);
    texture_rid = texture_rid_in;
    release_reservation = std::move(reservation);
  } catch (...) {
    return false;
  }
  if (!prior.is_valid() || prior == texture_rid_in) {
    return true;
  }
  defer_render_resource_rid_release(
      GodotRenderResourceKind::RenderingServerRid,
      prior,
      std::move(prior_reservation));
  return true;
}

void SharedLiveCpuTextureRidState::clear() {
  RenderResourceReleaseReservation no_reservation;
  (void)replace_rid(godot::RID(), no_reservation);
}

SharedLiveCpuTextureRidState::~SharedLiveCpuTextureRidState() {
  clear();
}

uint64_t register_live_cpu_display_wrapper_borrow(uint64_t stream_id) {
  if (stream_id == 0) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_live_cpu_display_wrapper_borrow_mutex);
  const uint64_t borrow_id = g_next_live_cpu_display_wrapper_borrow_id++;
  g_live_cpu_display_wrapper_borrows.emplace(borrow_id, CpuDisplayWrapperBorrow{stream_id});
  uint32_t& count = g_live_cpu_display_wrapper_borrow_counts[stream_id];
  if (count != UINT32_MAX) {
    count += 1u;
  }
  return borrow_id;
}

void set_live_cpu_display_wrapper_instance_id(uint64_t borrow_id, uint64_t wrapper_instance_id) {
  if (borrow_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_live_cpu_display_wrapper_borrow_mutex);
  const auto it = g_live_cpu_display_wrapper_borrows.find(borrow_id);
  if (it == g_live_cpu_display_wrapper_borrows.end()) {
    return;
  }
  it->second.wrapper_instance_id = wrapper_instance_id;
}

void unregister_live_cpu_display_wrapper_borrow(uint64_t borrow_id) {
  if (borrow_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_live_cpu_display_wrapper_borrow_mutex);
  const auto it = g_live_cpu_display_wrapper_borrows.find(borrow_id);
  if (it == g_live_cpu_display_wrapper_borrows.end()) {
    return;
  }
  const uint64_t stream_id = it->second.stream_id;
  g_live_cpu_display_wrapper_borrows.erase(it);
  auto count_it = g_live_cpu_display_wrapper_borrow_counts.find(stream_id);
  if (count_it == g_live_cpu_display_wrapper_borrow_counts.end()) {
    return;
  }
  if (count_it->second <= 1u) {
    g_live_cpu_display_wrapper_borrow_counts.erase(count_it);
    return;
  }
  count_it->second -= 1u;
}

bool has_live_cpu_display_wrapper_borrow(uint64_t stream_id) {
  if (stream_id == 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_live_cpu_display_wrapper_borrow_mutex);
  const auto it = g_live_cpu_display_wrapper_borrow_counts.find(stream_id);
  return it != g_live_cpu_display_wrapper_borrow_counts.end() && it->second > 0u;
}

void notify_live_cpu_display_wrapper_refresh(uint64_t stream_id, uint32_t width, uint32_t height) {
  if (stream_id == 0) {
    return;
  }

  std::vector<uint64_t> wrapper_instance_ids;
  {
    std::lock_guard<std::mutex> lock(g_live_cpu_display_wrapper_borrow_mutex);
    wrapper_instance_ids.reserve(g_live_cpu_display_wrapper_borrows.size());
    for (const auto& [borrow_id, borrow] : g_live_cpu_display_wrapper_borrows) {
      (void)borrow_id;
      if (borrow.stream_id == stream_id && borrow.wrapper_instance_id != 0) {
        wrapper_instance_ids.push_back(borrow.wrapper_instance_id);
      }
    }
  }

  for (uint64_t wrapper_instance_id : wrapper_instance_ids) {
    godot::Object* object = godot::ObjectDB::get_instance(wrapper_instance_id);
    LiveCpuDisplayTexture2D* wrapper = godot::Object::cast_to<LiveCpuDisplayTexture2D>(object);
    if (!wrapper) {
      continue;
    }
    wrapper->update_dimensions(width, height);
  }
}

DisplayDemandLease::~DisplayDemandLease() noexcept {
  release_display_demand_from_owner(lease_token_);
}

std::shared_ptr<DisplayDemandLease> retain_display_demand_lease(
    uint64_t stream_id) {
  if (stream_id == 0) {
    return {};
  }
  const uint64_t lease_token = retain_display_demand_for_owner(stream_id);
  if (lease_token == 0) {
    return {};
  }
  try {
    std::shared_ptr<DisplayDemandLease> lease =
        std::make_shared<DisplayDemandLease>(lease_token);
    return lease;
  } catch (...) {
    release_display_demand_from_owner(lease_token);
    return {};
  }
}

void install_display_demand_release_dispatcher(CamBANGServer* server) noexcept {
  g_display_demand_dispatcher.install(
      server, retain_display_demand_target, release_display_demand_target);
}

void uninstall_display_demand_release_dispatcher(CamBANGServer* server) noexcept {
  g_display_demand_dispatcher.uninstall(server);
}

LiveCpuDisplayTexture2D::~LiveCpuDisplayTexture2D() {
  clear_runtime_references_();
}

void LiveCpuDisplayTexture2D::init(
    std::shared_ptr<SharedLiveCpuTextureRidState> state,
    uint64_t stream_id,
    uint32_t width,
    uint32_t height,
    bool retain_display_demand) {
  clear_runtime_references_();
  state_ = std::move(state);
  stream_id_ = stream_id;
  width_ = width;
  height_ = height;
  borrow_id_ = register_live_cpu_display_wrapper_borrow(stream_id_);
  set_live_cpu_display_wrapper_instance_id(borrow_id_, static_cast<uint64_t>(get_instance_id()));
  if (retain_display_demand && stream_id_ != 0) {
    display_demand_lease_ = retain_display_demand_lease(stream_id_);
  }
  if (display_demand_trace_enabled()) {
    godot::UtilityFunctions::print(
        "[CamBANG][DemandTrace] live_cpu_display_view_create stream_id=",
        static_cast<uint64_t>(stream_id_),
        " wrapper_id=",
        static_cast<uint64_t>(get_instance_id()),
        " borrow_id=",
        static_cast<uint64_t>(borrow_id_),
        " persistent_demand=",
        retain_display_demand);
  }
}

void LiveCpuDisplayTexture2D::update_dimensions(uint32_t width, uint32_t height) {
  width_ = width;
  height_ = height;
  emit_changed();
}

int32_t LiveCpuDisplayTexture2D::_get_width() const {
  return static_cast<int32_t>(width_);
}

int32_t LiveCpuDisplayTexture2D::_get_height() const {
  return static_cast<int32_t>(height_);
}

bool LiveCpuDisplayTexture2D::_is_pixel_opaque(int32_t x, int32_t y) const {
  (void)x;
  (void)y;
  return false;
}

bool LiveCpuDisplayTexture2D::_has_alpha() const {
  return true;
}

void LiveCpuDisplayTexture2D::_draw(
    const godot::RID& to_canvas_item,
    const godot::Vector2& pos,
    const godot::Color& modulate,
    bool transpose) const {
  const godot::RID texture_rid = state_ ? state_->snapshot_rid() : godot::RID();
  if (!texture_rid.is_valid()) {
    return;
  }
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs) {
    return;
  }
  rs->canvas_item_add_texture_rect(
      to_canvas_item,
      godot::Rect2(pos, godot::Vector2(static_cast<float>(width_), static_cast<float>(height_))),
      texture_rid,
      false,
      modulate,
      transpose);
}

void LiveCpuDisplayTexture2D::_draw_rect(
    const godot::RID& to_canvas_item,
    const godot::Rect2& rect,
    bool tile,
    const godot::Color& modulate,
    bool transpose) const {
  const godot::RID texture_rid = state_ ? state_->snapshot_rid() : godot::RID();
  if (!texture_rid.is_valid()) {
    return;
  }
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs) {
    return;
  }
  rs->canvas_item_add_texture_rect(
      to_canvas_item,
      rect,
      texture_rid,
      tile,
      modulate,
      transpose);
}

void LiveCpuDisplayTexture2D::_draw_rect_region(
    const godot::RID& to_canvas_item,
    const godot::Rect2& rect,
    const godot::Rect2& src_rect,
    const godot::Color& modulate,
    bool transpose,
    bool clip_uv) const {
  const godot::RID texture_rid = state_ ? state_->snapshot_rid() : godot::RID();
  if (!texture_rid.is_valid()) {
    return;
  }
  godot::RenderingServer* rs = godot::RenderingServer::get_singleton();
  if (!rs) {
    return;
  }
  rs->canvas_item_add_texture_rect_region(
      to_canvas_item,
      rect,
      texture_rid,
      src_rect,
      modulate,
      transpose,
      clip_uv);
}

godot::RID LiveCpuDisplayTexture2D::_get_rid() const {
  return state_ ? state_->snapshot_rid() : godot::RID();
}

void LiveCpuDisplayTexture2D::clear_runtime_references_() {
  unregister_live_cpu_display_wrapper_borrow(borrow_id_);
  borrow_id_ = 0;
  display_demand_lease_.reset();
  state_.reset();
  stream_id_ = 0;
}

void register_stream_result_internal_classes() {
  godot::ClassDB::register_class<LiveCpuDisplayTexture2D>();
}

} // namespace cambang
