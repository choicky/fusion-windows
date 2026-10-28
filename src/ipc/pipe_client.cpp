#include "ipc/pipe_client.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace fusion::ipc {

PipeClient::PipeClient(std::wstring pipe_name) : pipe_name_(std::move(pipe_name)) {}

PipeClient::~PipeClient() { disconnect(); }

bool PipeClient::connect(std::chrono::milliseconds timeout, ConnectFailure *failure) {
  std::lock_guard lock(mutex_);
  if (connected_)
    return true;

  // Every failure below records what failed and the Win32 code, captured at the
  // call itself: an empty name, a refused open and a refused identity check all
  // end up as "the Host is unreachable" without this.
  const auto fail = [failure](ConnectStage stage, unsigned long error) {
    if (failure) {
      failure->stage = stage;
      failure->win32_error = error;
    }
    return false;
  };

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    handle_ = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (handle_ != INVALID_HANDLE_VALUE) {
      DWORD mode = PIPE_READMODE_BYTE;
      if (!SetNamedPipeHandleState(handle_, &mode, nullptr, nullptr)) {
        const unsigned long error = GetLastError();
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        return fail(ConnectStage::PipeMode, error);
      }
      if (!verify_server_identity(handle_, failure)) {
        if (failure)
          failure->stage = ConnectStage::Identity;
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        return false;
      }
      connected_ = true;
      return true;
    }
    const unsigned long err = GetLastError();
    if (err != ERROR_PIPE_BUSY)
      return fail(pipe_name_.empty() ? ConnectStage::PipeName : ConnectStage::Open, err);
    if (std::chrono::steady_clock::now() >= deadline)
      return fail(ConnectStage::Open, err);
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                         deadline - std::chrono::steady_clock::now())
                         .count();
    if (remaining <= 0)
      return fail(ConnectStage::Open, err);
    if (!WaitNamedPipeW(pipe_name_.c_str(), static_cast<DWORD>(remaining)))
      return fail(ConnectStage::Open, GetLastError());
  }
}

void PipeClient::disconnect() {
  std::lock_guard lock(mutex_);
  if (handle_ != INVALID_HANDLE_VALUE) {
    CancelIo(handle_);
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }
  connected_ = false;
}

bool PipeClient::connected() const {
  std::lock_guard lock(mutex_);
  return connected_;
}

void *PipeClient::native_handle() const {
  std::lock_guard lock(mutex_);
  return connected_ ? handle_ : INVALID_HANDLE_VALUE;
}

namespace {

// A pipe that cannot be written at all. On these errors the first write attempt
// is rejected outright, so no byte of the frame was accepted. Every other
// failure — in particular an overlapped write that was pending and then failed —
// is treated as uncertain: an aborted pending write reports zero transferred
// bytes even when the pipe already held part of the frame (measured on this
// machine: the peer could PeekNamedPipe those bytes).
bool connection_unusable(DWORD error) {
  return error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA || error == ERROR_PIPE_NOT_CONNECTED;
}

} // namespace

WriteOutcome PipeClient::write_all(const void *data, std::size_t size, unsigned long *error) {
  const auto *ptr = static_cast<const std::uint8_t *>(data);
  std::size_t written_total = 0;
  // Only a proven zero-byte outcome may be reported as NotWritten. A frame can be
  // written in several chunks, and a write that the transport accepted and that
  // only failed later can no longer be distinguished from one that delivered
  // bytes, so anything unproven becomes Uncertain.
  //
  // `error` is filled at the failing call, never after the cleanup below: the
  // cancellation and the handle close each leave their own code behind.
  while (written_total < size) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
      if (error)
        *error = GetLastError();
      return written_total == 0 ? WriteOutcome::NotWritten : WriteOutcome::Uncertain;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(handle_, ptr + written_total, static_cast<DWORD>(size - written_total),
                        &written, &ov);
    if (ok) {
      CloseHandle(ov.hEvent);
      written_total += written;
      continue;
    }
    const DWORD err = GetLastError();
    if (err != ERROR_IO_PENDING) {
      // The request never reached the transport queue: with no byte written by an
      // earlier chunk and a dead pipe this is a definite zero-byte failure.
      CloseHandle(ov.hEvent);
      if (error)
        *error = err;
      if (written_total == 0 && connection_unusable(err))
        return WriteOutcome::NotWritten;
      return WriteOutcome::Uncertain;
    }
    const DWORD wait_result =
        WaitForSingleObject(ov.hEvent, static_cast<DWORD>(kRequestTimeout.count()));
    if (wait_result != WAIT_OBJECT_0) {
      const unsigned long code = wait_result == WAIT_TIMEOUT
                                     ? transport_timeout_error
                                     : (wait_result == WAIT_FAILED ? GetLastError()
                                                                   : ERROR_GEN_FAILURE);
      CancelIoEx(handle_, &ov);
      GetOverlappedResult(handle_, &ov, &written, TRUE);
      written_total += written;
      CloseHandle(ov.hEvent);
      if (error)
        *error = code;
      return WriteOutcome::Uncertain;
    }
    if (!GetOverlappedResult(handle_, &ov, &written, FALSE)) {
      // A pending write that fails may already have delivered part of the frame.
      const unsigned long code = GetLastError();
      CloseHandle(ov.hEvent);
      if (error)
        *error = code;
      return WriteOutcome::Uncertain;
    }
    CloseHandle(ov.hEvent);
    written_total += written;
  }
  return WriteOutcome::Written;
}

bool PipeClient::read_all_overlapped(void *data, std::size_t size,
                                     std::chrono::milliseconds timeout, unsigned long *error) {
  // Every return below reports the code of the call that failed, captured before the
  // cancellation and the handle close — those overwrite the thread's last-error slot
  // with ERROR_OPERATION_ABORTED and friends, which is why a caller reading
  // GetLastError() after this function used to see 995 instead of the real cause.
  const auto fail = [error](unsigned long code) {
    if (error)
      *error = code;
    return false;
  };

  auto *ptr = static_cast<std::uint8_t *>(data);
  std::size_t read_total = 0;
  while (read_total < size) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
      return fail(GetLastError());
    DWORD bytes_read = 0;
    BOOL ok = ReadFile(handle_, ptr + read_total, static_cast<DWORD>(size - read_total),
                       &bytes_read, &ov);
    if (ok) {
      CloseHandle(ov.hEvent);
      // A completed zero-byte read on a byte-mode pipe is the other end closing,
      // which is a broken pipe rather than "nothing to report".
      if (bytes_read == 0)
        return fail(ERROR_BROKEN_PIPE);
      read_total += bytes_read;
      continue;
    }
    const DWORD read_error = GetLastError();
    if (read_error != ERROR_IO_PENDING) {
      CloseHandle(ov.hEvent);
      return fail(read_error);
    }
    DWORD wait_ms = (timeout.count() == 0) ? INFINITE : static_cast<DWORD>(timeout.count());
    const DWORD wait_result = WaitForSingleObject(ov.hEvent, wait_ms);
    if (wait_result != WAIT_OBJECT_0) {
      // The deadline is the failure, and it is reported as one code everywhere.
      const unsigned long code = wait_result == WAIT_TIMEOUT
                                     ? transport_timeout_error
                                     : (wait_result == WAIT_FAILED ? GetLastError()
                                                                   : ERROR_GEN_FAILURE);
      CancelIoEx(handle_, &ov);
      GetOverlappedResult(handle_, &ov, &bytes_read, TRUE);
      CloseHandle(ov.hEvent);
      return fail(code);
    }
    if (!GetOverlappedResult(handle_, &ov, &bytes_read, FALSE)) {
      const unsigned long code = GetLastError();
      CloseHandle(ov.hEvent);
      return fail(code);
    }
    CloseHandle(ov.hEvent);
    if (bytes_read == 0)
      return fail(ERROR_BROKEN_PIPE);
    read_total += bytes_read;
  }
  return true;
}

WriteOutcome PipeClient::send_frame(const WireFrame &frame, unsigned long *error) {
  std::lock_guard lock(mutex_);
  if (!connected_)
    return WriteOutcome::NotWritten;
  auto wire = frame_message(frame.type, frame.payload);
  const WriteOutcome outcome = write_all(wire.data(), wire.size(), error);
  if (outcome != WriteOutcome::Written)
    connected_ = false;
  return outcome;
}

std::optional<WireFrame> PipeClient::recv_frame(std::chrono::milliseconds timeout,
                                                unsigned long *error) {
  std::lock_guard lock(mutex_);
  if (!connected_)
    return std::nullopt;

  std::uint8_t header_buf[header_size];
  if (!read_all_overlapped(header_buf, header_size, timeout, error)) {
    connected_ = false;
    return std::nullopt;
  }

  std::uint32_t payload_len = 0;
  std::memcpy(&payload_len, header_buf, 4);
  if (payload_len > max_message_payload) {
    connected_ = false;
    return std::nullopt;
  }

  auto type_byte = header_buf[4];
  if (type_byte < 0x01 || type_byte > max_message_type) {
    connected_ = false;
    return std::nullopt;
  }

  WireFrame frame;
  frame.type = static_cast<MessageType>(type_byte);
  if (payload_len > 0) {
    frame.payload.resize(payload_len);
    if (!read_all_overlapped(frame.payload.data(), payload_len, timeout, error)) {
      connected_ = false;
      return std::nullopt;
    }
  }
  return frame;
}

WriteOutcome PipeClient::send_request(MessageType type, const std::vector<std::uint8_t> &payload,
                                      unsigned long *error) {
  WireFrame frame;
  frame.type = type;
  frame.payload = payload;
  return send_frame(frame, error);
}

std::optional<WireFrame> PipeClient::recv_response(std::chrono::milliseconds timeout,
                                                   unsigned long *error) {
  return recv_frame(timeout, error);
}

} // namespace fusion::ipc
