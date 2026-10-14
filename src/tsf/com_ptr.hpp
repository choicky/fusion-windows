#pragma once

// Minimal COM reference pointer for the Stage 2.5 TSF shell (no ATL dependency).

#include <unknwn.h>

#include <utility>

namespace fusion::tsf {

template <typename T> class ComRef {
public:
  ComRef() = default;
  explicit ComRef(T *ptr) : ptr_(ptr) {}
  ComRef(const ComRef &) = delete;
  ComRef &operator=(const ComRef &) = delete;
  ComRef(ComRef &&other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
  ComRef &operator=(ComRef &&other) noexcept {
    if (this != &other) {
      reset();
      ptr_ = other.ptr_;
      other.ptr_ = nullptr;
    }
    return *this;
  }
  ~ComRef() { reset(); }
  void reset() {
    if (ptr_) {
      ptr_->Release();
      ptr_ = nullptr;
    }
  }
  T *get() const { return ptr_; }
  T *operator->() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }
  T **out() {
    reset();
    return &ptr_;
  }
  // QI helper: queries for U and returns a ComRef; empty on failure.
  template <typename U, typename IID_T> ComRef<U> as(const IID_T &iid) const {
    U *raw = nullptr;
    if (!ptr_ || FAILED(ptr_->QueryInterface(iid, reinterpret_cast<void **>(&raw)))) {
      return ComRef<U>();
    }
    return ComRef<U>(raw);
  }

private:
  T *ptr_ = nullptr;
};

} // namespace fusion::tsf
