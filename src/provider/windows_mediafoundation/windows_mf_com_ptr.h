// src/provider/windows_mediafoundation/windows_mf_com_ptr.h
#pragma once

#ifdef _WIN32

#include <utility>

namespace cambang {

// Minimal COM smart pointer (dev-only). Avoids pulling in WRL/ATL.
// - Copy = AddRef
// - Move = transfer
template <typename T>
class ComPtr {
public:
  ComPtr() = default;
  ComPtr(std::nullptr_t) {}

  explicit ComPtr(T* p) : p_(p) {}

  ComPtr(const ComPtr& o) : p_(o.p_) { add_ref_(); }
  ComPtr& operator=(const ComPtr& o) {
    if (this == &o) return *this;
    reset();
    p_ = o.p_;
    add_ref_();
    return *this;
  }

  ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  ComPtr& operator=(ComPtr&& o) noexcept {
    if (this == &o) return *this;
    reset();
    p_ = o.p_;
    o.p_ = nullptr;
    return *this;
  }

  ~ComPtr() { reset(); }

  void reset(T* p = nullptr) {
    if (p_) {
      p_->Release();
    }
    p_ = p;
  }

  T* get() const { return p_; }
  T** put() {
    reset();
    return &p_;
  }

  T* operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }

  // For APIs that expect a pre-existing pointer without releasing.
  T** addressof() { return &p_; }

private:
  void add_ref_() {
    if (p_) p_->AddRef();
  }

  T* p_ = nullptr;
};

} // namespace cambang

#endif // _WIN32
