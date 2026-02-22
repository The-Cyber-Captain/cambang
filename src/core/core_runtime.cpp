// src/core/core_runtime.cpp

#include "core/core_runtime.h"

#include <cassert>

#include <utility>

namespace cambang {

CoreRuntime::CoreRuntime()
    : core_thread_(),
      streams_(),
      snapshot_seq_(0),
      publisher_(),
      dispatcher_(&streams_),
      ingress_(&core_thread_, [this](CoreCommand&& cmd) {
        // This lambda is executed ONLY on the core thread (posted by ingress).
        assert(core_thread_.is_core_thread());
        dispatcher_.dispatch(std::move(cmd));
      }) {}

CoreRuntime::~CoreRuntime() {
  stop();
}

bool CoreRuntime::start() {
  publish_pending_.store(false, std::memory_order_relaxed);
  publish_requests_coalesced_.store(0, std::memory_order_relaxed);
  publish_requests_dropped_full_.store(0, std::memory_order_relaxed);
  publish_requests_dropped_closed_.store(0, std::memory_order_relaxed);
  publish_requests_dropped_allocfail_.store(0, std::memory_order_relaxed);
  return core_thread_.start(this);
}

void CoreRuntime::stop() {
  core_thread_.stop();
}

CoreRuntime::Stats CoreRuntime::stats_copy() const noexcept {
  Stats s;
  s.publish_requests_coalesced = publish_requests_coalesced_.load(std::memory_order_relaxed);
  s.publish_requests_dropped_full = publish_requests_dropped_full_.load(std::memory_order_relaxed);
  s.publish_requests_dropped_closed = publish_requests_dropped_closed_.load(std::memory_order_relaxed);
  s.publish_requests_dropped_allocfail = publish_requests_dropped_allocfail_.load(std::memory_order_relaxed);
  return s;
}

void CoreRuntime::request_publish() {
  // Coalesce publish requests to avoid spamming the core mailbox.
  const bool was_pending = publish_pending_.exchange(true, std::memory_order_acq_rel);
  if (was_pending) {
    publish_requests_coalesced_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  const CoreThread::PostResult r = core_thread_.try_post([this]() {
    assert(core_thread_.is_core_thread());

    // Clear pending first so a new request can enqueue even if publish work is heavy.
    publish_pending_.store(false, std::memory_order_release);

    CoreSnapshot snap;
    snap.seq = ++snapshot_seq_;

    snap.streams.reserve(streams_.all().size());
    for (const auto& kv : streams_.all()) {
      const auto& rec = kv.second;
      CoreSnapshot::Stream s;
      s.stream_id = rec.stream_id;
      s.created = rec.created;
      s.started = rec.started;
      s.frames_received = rec.frames_received;
      s.frames_released = rec.frames_released;
      s.frames_dropped = rec.frames_dropped;
      s.last_error_code = static_cast<std::int32_t>(rec.last_error_code);
      snap.streams.push_back(s);
    }

    publisher_.publish(std::move(snap));
  });

  if (r == CoreThread::PostResult::Enqueued) {
    return;
  }

  // Failed to enqueue: clear pending so callers can retry later.
  publish_pending_.store(false, std::memory_order_release);

  switch (r) {
    case CoreThread::PostResult::QueueFull:
      publish_requests_dropped_full_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CoreThread::PostResult::Closed:
      publish_requests_dropped_closed_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CoreThread::PostResult::AllocFail:
      publish_requests_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CoreThread::PostResult::Enqueued:
      break;
  }
}

} // namespace cambang

