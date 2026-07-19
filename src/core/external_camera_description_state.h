#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "core/camera_concurrency_adc.h"
#include "core/camera_fact_types.h"

namespace cambang {

struct ExternalCameraDescriptionEntry {
  std::string camera_id;
  CameraStaticFacts facts;
};

// Typed configured/active external facts. Camera identity remains outside the
// fact bundle so exact matching is not mistaken for a descriptive camera fact.
class ExternalCameraDescriptionState final {
 public:
  using Entries = std::unordered_map<std::string, ExternalCameraDescriptionEntry>;

  const ExternalCameraDescriptionEntry* find_exact(const std::string& camera_id) const noexcept {
    const auto it = entries_.find(camera_id);
    return it == entries_.end() ? nullptr : &it->second;
  }

  const Entries& entries() const noexcept { return entries_; }
  const std::optional<camera_concurrency::Truth>& concurrency() const noexcept {
    return concurrency_;
  }

  void replace(Entries entries, std::optional<camera_concurrency::Truth> concurrency) {
    entries_ = std::move(entries);
    concurrency_ = std::move(concurrency);
  }

 private:
  Entries entries_;
  std::optional<camera_concurrency::Truth> concurrency_;
};

} // namespace cambang
