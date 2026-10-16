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

bool PipeClient::connect(std::chrono::milliseconds timeout) {
  std::lock_guard lock(mutex_);
  if (connected_)
    return true;

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    handle_ =
        CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED, nullptr);
    if (handle_ != INVALID_HANDLE_VALUE) {
      DWORD mode = PIPE_READMODE_BYTE;
      if (!SetNamedPipeHandleState(handle_, &mode, nullptr, nullptr)) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        return false;
      }
      if (!verify_server_identity(handle_)) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        return false;
      }
      connected_ = true;
      return true;
    }
    auto err = GetLastError();
    if (err != ERROR_PIPE_BUSY)
      return false;
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                         deadline - std::chrono::steady_clock::now())
                         .count();
    if (remaining <= 0)
      return false;
    if (!WaitNamedPipeW(pipe_name_.c_str(), static_cast<DWORD>(remaining)))
      return false;
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

bool PipeClient::write_all(const void *data, std::size_t size) {
  const auto *ptr = static_cast<const std::uint8_t *>(data);
  std::size_t written_total = 0;
  while (written_total < size) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
      return false;
    DWORD written = 0;
    BOOL ok = WriteFile(handle_, ptr + written_total,
                        static_cast<DWORD>(size - written_total), &written, &ov);
    if (ok) {
      CloseHandle(ov.hEvent);
      written_total += written;
      continue;
    }
    auto err = GetLastError();
    if (err != ERROR_IO_PENDING) {
      CloseHandle(ov.hEvent);
      return false;
    }
    auto result = WaitForSingleObject(ov.hEvent, static_cast<DWORD>(kRequestTimeout.count()));
    if (result != WAIT_OBJECT_0) {
      CancelIoEx(handle_, &ov);
      GetOverlappedResult(handle_, &ov, &written, TRUE);
      CloseHandle(ov.hEvent);
      return false;
    }
    if (!GetOverlappedResult(handle_, &ov, &written, FALSE)) {
      CloseHandle(ov.hEvent);
      return false;
    }
    CloseHandle(ov.hEvent);
    written_total += written;
  }
  return true;
}

bool PipeClient::read_all_overlapped(void *data, std::size_t size,
                                     std::chrono::milliseconds timeout) {
  auto *ptr = static_cast<std::uint8_t *>(data);
  std::size_t read_total = 0;
  while (read_total < size) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
      return false;
    DWORD bytes_read = 0;
    BOOL ok = ReadFile(handle_, ptr + read_total, static_cast<DWORD>(size - read_total),
                       &bytes_read, &ov);
    if (ok) {
      CloseHandle(ov.hEvent);
      if (bytes_read == 0)
        return false;
      read_total += bytes_read;
      continue;
    }
    auto err = GetLastError();
    if (err != ERROR_IO_PENDING) {
      CloseHandle(ov.hEvent);
      return false;
    }
    DWORD wait_ms = (timeout.count() == 0) ? INFINITE : static_cast<DWORD>(timeout.count());
    auto result = WaitForSingleObject(ov.hEvent, wait_ms);
    if (result != WAIT_OBJECT_0) {
      CancelIoEx(handle_, &ov);
      GetOverlappedResult(handle_, &ov, &bytes_read, TRUE);
      CloseHandle(ov.hEvent);
      return false;
    }
    if (!GetOverlappedResult(handle_, &ov, &bytes_read, FALSE)) {
      CloseHandle(ov.hEvent);
      return false;
    }
    CloseHandle(ov.hEvent);
    if (bytes_read == 0)
      return false;
    read_total += bytes_read;
  }
  return true;
}

bool PipeClient::send_frame(const WireFrame &frame) {
  std::lock_guard lock(mutex_);
  if (!connected_)
    return false;
  auto wire = frame_message(frame.type, frame.payload);
  if (!write_all(wire.data(), wire.size())) {
    connected_ = false;
    return false;
  }
  return true;
}

std::optional<WireFrame> PipeClient::recv_frame(std::chrono::milliseconds timeout) {
  std::lock_guard lock(mutex_);
  if (!connected_)
    return std::nullopt;

  std::uint8_t header_buf[header_size];
  if (!read_all_overlapped(header_buf, header_size, timeout)) {
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
  if (type_byte < 0x01 || type_byte > 0x0A) {
    connected_ = false;
    return std::nullopt;
  }

  WireFrame frame;
  frame.type = static_cast<MessageType>(type_byte);
  if (payload_len > 0) {
    frame.payload.resize(payload_len);
    if (!read_all_overlapped(frame.payload.data(), payload_len, timeout)) {
      connected_ = false;
      return std::nullopt;
    }
  }
  return frame;
}

bool PipeClient::send_request(MessageType type, const std::vector<std::uint8_t> &payload) {
  WireFrame frame;
  frame.type = type;
  frame.payload = payload;
  return send_frame(frame);
}

std::optional<WireFrame> PipeClient::recv_response(std::chrono::milliseconds timeout) {
  return recv_frame(timeout);
}

} // namespace fusion::ipc
