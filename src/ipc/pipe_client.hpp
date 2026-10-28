#pragma once

// Windows Named Pipe client transport for Stage 4A IPC.
// Thread-safe: internal mutex serializes all operations.
// Uses FILE_FLAG_OVERLAPPED for request timeout via WaitForSingleObject.
// On any error or timeout, marks disconnected; all subsequent operations fail.

#include "fusion/ipc_protocol.hpp"
#include "ipc/pipe_security.hpp"
#include "win32/srw_mutex.hpp"

#include <chrono>
#include <mutex>
#include <string>

namespace fusion::ipc {

// UNFROZEN engineering constant — matches FcitxBackend::invoke request_wait (2s).
inline constexpr auto kRequestTimeout = std::chrono::milliseconds(2000);

// Outcome of writing one frame. A frame may be written in several chunks, and an
// overlapped write that the transport accepted and that only fails later cannot be
// distinguished from one that delivered bytes, so a caller that needs to know
// whether a request can still be treated as "never delivered" must use this result
// instead of a bool.
enum class WriteOutcome {
  NotWritten, // provably no byte of this frame was accepted (not connected, or the
              // first write attempt was rejected outright by an unusable pipe)
  Written,    // the whole frame was accepted by the transport
  Uncertain,  // bytes may have been accepted; zero-byte status cannot be proven
};

class PipeClient {
public:
  explicit PipeClient(std::wstring pipe_name = compute_pipe_name());
  ~PipeClient();

  PipeClient(const PipeClient &) = delete;
  PipeClient &operator=(const PipeClient &) = delete;

  // connect_timeout: max wait for ERROR_PIPE_BUSY (UNFROZEN engineering constant).
  // `failure` (optional, diagnostics only) receives the stage that failed and the
  // Win32 code of the call that failed.
  bool connect(std::chrono::milliseconds connect_timeout = std::chrono::milliseconds(200),
               ConnectFailure *failure = nullptr);
  void disconnect();
  bool connected() const;

  WriteOutcome send_frame(const WireFrame &frame, unsigned long *error = nullptr);
  // recv_timeout == 0 means block indefinitely (for idle wait on server side).
  std::optional<WireFrame> recv_frame(std::chrono::milliseconds recv_timeout = kRequestTimeout,
                                      unsigned long *error = nullptr);

  WriteOutcome send_request(MessageType type, const std::vector<std::uint8_t> &payload,
                            unsigned long *error = nullptr);
  std::optional<WireFrame> recv_response(std::chrono::milliseconds timeout = kRequestTimeout,
                                         unsigned long *error = nullptr);

  void *native_handle() const;

private:
  std::wstring pipe_name_;
  void *handle_ = reinterpret_cast<void *>(-1); // INVALID_HANDLE_VALUE
  // Not std::mutex: this class is compiled into the TIP DLL, which must not hold
  // CRT-owned synchronization state (see win32/srw_mutex.hpp).
  mutable win32::SrwMutex mutex_;
  bool connected_ = false;

  // `error` (optional) receives the Win32 code of the call that failed, captured
  // *before* CancelIoEx / GetOverlappedResult / CloseHandle run: those three leave
  // their own codes in the thread's last-error slot, so a caller that asked
  // GetLastError() afterwards reported the cleanup instead of the cause. A deadline
  // expiry reports transport_timeout_error (see fusion/backend.hpp).
  WriteOutcome write_all(const void *data, std::size_t size, unsigned long *error);
  bool read_all_overlapped(void *data, std::size_t size, std::chrono::milliseconds timeout,
                           unsigned long *error);
};

} // namespace fusion::ipc
