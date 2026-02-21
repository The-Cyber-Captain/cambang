// src/core/core_runtime.cpp

#include "core/core_runtime.h"

#include <cassert>

namespace cambang {

CoreRuntime::CoreRuntime()
    : core_thread_(),
      dispatcher_(),
      ingress_(&core_thread_, [this](CoreCommand&& cmd) {
        // This lambda is executed ONLY on the core thread (posted by ingress).
        assert(core_thread_.is_core_thread());
        dispatcher_.dispatch(std::move(cmd));
      }) {}

CoreRuntime::~CoreRuntime() {
  stop();
}

bool CoreRuntime::start() {
  return core_thread_.start(this);
}

void CoreRuntime::stop() {
  core_thread_.stop();
}

} // namespace cambang
