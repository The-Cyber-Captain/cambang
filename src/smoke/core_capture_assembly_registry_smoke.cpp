#include <cassert>
#include <cstdint>
#include <iostream>

#include "core/core_capture_assembly_registry.h"

using namespace cambang;

int main() {
#if !defined(CAMBANG_INTERNAL_SMOKE)
#error "core_capture_assembly_registry_smoke: build with -DCAMBANG_INTERNAL_SMOKE=1"
#endif

  CoreCaptureAssemblyRegistry reg;

  // frame/default-image retained before capture_completed => assembled-success eligible internally.
  reg.mark_default_image_retained(100, 10);
  reg.mark_capture_completed(100, 10);
  auto a = reg.find_for_smoke(100, 10);
  assert(a);
  assert(a->has_default_image_retained);
  assert(a->terminal_state == CoreCaptureAssemblyRegistry::TerminalState::COMPLETED);
  assert(!a->has_failure_error_code);
  assert(reg.is_assembly_successful(100, 10));

  // capture_completed before frame => completed terminal; image-bearing once frame arrives.
  reg.mark_capture_completed(101, 11);
  auto b0 = reg.find_for_smoke(101, 11);
  assert(b0);
  assert(!b0->has_default_image_retained);
  assert(b0->terminal_state == CoreCaptureAssemblyRegistry::TerminalState::COMPLETED);
  reg.mark_default_image_retained(101, 11);
  auto b1 = reg.find_for_smoke(101, 11);
  assert(b1);
  assert(b1->has_default_image_retained);
  assert(b1->terminal_state == CoreCaptureAssemblyRegistry::TerminalState::COMPLETED);
  assert(!reg.is_assembly_successful(101, 999));
  assert(reg.is_assembly_successful(101, 11));

  // capture_failed before frame => failed terminal.
  reg.mark_capture_failed(102, 12, 77);
  auto c = reg.find_for_smoke(102, 12);
  assert(c);
  assert(!c->has_default_image_retained);
  assert(c->terminal_state == CoreCaptureAssemblyRegistry::TerminalState::FAILED);
  assert(c->has_failure_error_code);
  assert(c->failure_error_code == 77);
  assert(!reg.is_assembly_successful(102, 12));

  // frame then capture_failed => failed terminal, not success.
  reg.mark_default_image_retained(103, 13);
  reg.mark_capture_failed(103, 13, 78);
  auto d = reg.find_for_smoke(103, 13);
  assert(d);
  assert(d->has_default_image_retained);
  assert(d->terminal_state == CoreCaptureAssemblyRegistry::TerminalState::FAILED);
  assert(d->has_failure_error_code);
  assert(d->failure_error_code == 78);
  assert(!reg.is_assembly_successful(103, 13));

  // capture_completed without frame => terminal completed, not image-bearing.
  reg.mark_capture_completed(104, 14);
  auto e = reg.find_for_smoke(104, 14);
  assert(e);
  assert(!e->has_default_image_retained);
  assert(e->terminal_state == CoreCaptureAssemblyRegistry::TerminalState::COMPLETED);
  assert(!reg.is_assembly_successful(104, 14));

  // frame without completed/failed => not yet terminal eligible.
  reg.mark_default_image_retained(105, 15);
  auto f = reg.find_for_smoke(105, 15);
  assert(f);
  assert(f->has_default_image_retained);
  assert(f->terminal_state == CoreCaptureAssemblyRegistry::TerminalState::NONE);
  assert(!reg.is_assembly_successful(105, 15));

  // isolation across unrelated capture/device pairs.
  auto miss_capture = reg.find_for_smoke(999, 15);
  auto miss_device = reg.find_for_smoke(105, 999);
  assert(!miss_capture);
  assert(!miss_device);

  std::cout << "PASS core_capture_assembly_registry_smoke\n";
  return 0;
}
