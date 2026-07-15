#pragma once

#include <memory>
#include <utility>

#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

inline bool frame_has_release_token(const FrameView& frame) noexcept {
  return frame.release_owner || frame.release || frame.release_user != nullptr;
}

inline void adopt_singular_frame_release_owner(FrameView& frame) {
  if (frame.release_owner || (!frame.release && frame.release_user == nullptr)) {
    return;
  }

  auto owner = std::make_shared<FrameReleaseOwner>();
  owner->callback_view = frame;
  owner->callback_view.release = nullptr;
  owner->callback_view.release_user = nullptr;
  owner->callback_view.release_owner.reset();
  owner->release = frame.release;
  owner->release_user = frame.release_user;
  owner->stream_id = frame.stream_id;
  owner->acquisition_session_id = frame.acquisition_session_id;

  frame.release_owner = std::move(owner);
  frame.release = nullptr;
  frame.release_user = nullptr;
}

template <typename OnThrowFn>
OwnedFrameReleaseResult release_owned_frame_once(
    FrameView& frame,
    OnThrowFn&& on_throw) noexcept {
  if (frame.release_owner) {
    std::shared_ptr<FrameReleaseOwner> owner = std::move(frame.release_owner);
    frame.release = nullptr;
    frame.release_user = nullptr;
    return owner->release_once(std::forward<OnThrowFn>(on_throw));
  }

  if (!frame.release && frame.release_user == nullptr) {
    return OwnedFrameReleaseResult::Noop;
  }

  FrameView owned = frame;
  frame.release = nullptr;
  frame.release_user = nullptr;

  OwnedFrameReleaseResult result = OwnedFrameReleaseResult::Released;
  try {
    if (owned.release) {
      owned.release(owned.release_user, &owned);
    }
  } catch (...) {
    result = OwnedFrameReleaseResult::ReleaseCallbackThrew;
    on_throw();
  }
  return result;
}

} // namespace cambang
