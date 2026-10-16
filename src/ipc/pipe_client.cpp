#include "ipc/pipe_client.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
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
                    0, nullptr);
    if (handle_ != INVALID_HANDLE_VALUE) {
      DWORD mode = PIPE_READMODE_BYTE;
      if (!SetNamedPipeHandleState(handle_, &mode, nullptr, nullptr)) {
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
    if (!WaitNamedPipeW(pipe_name_.c_str(),
                        static_cast<DWORD>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                                  std::chrono::steady_clock::now())
                                .count())))
      return false;
  }
}

void PipeClient::disconnect() {
  std::lock_guard lock(mutex_);
  if (connected_) {
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
    connected_ = false;
  }
}

bool PipeClient::connected() const {
  std::lock_guard lock(mutex_);
  return connected_;
}

bool PipeClient::write_all(const void *data, std::size_t size) {
  const auto *ptr = static_cast<const std::uint8_t *>(data);
  std::size_t written_total = 0;
  while (written_total < size) {
    DWORD written = 0;
    if (!WriteFile(handle_, ptr + written_total,
                   static_cast<DWORD>(size - written_total), &written, nullptr))
      return false;
    written_total += written;
  }
  return true;
}

bool PipeClient::wait_for_data(std::chrono::milliseconds timeout) {
  DWORD bytes_available = 0;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    if (!PeekNamedPipe(handle_, nullptr, 0, nullptr, &bytes_available, nullptr))
      return false;
    if (bytes_available > 0)
      return true;
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    Sleep(1);
  }
}

bool PipeClient::read_all(void *data, std::size_t size, std::chrono::milliseconds timeout) {
  if (!wait_for_data(timeout))
    return false;
  auto *ptr = static_cast<std::uint8_t *>(data);
  std::size_t read_total = 0;
  while (read_total < size) {
    DWORD bytes_read = 0;
    if (!ReadFile(handle_, ptr + read_total, static_cast<DWORD>(size - read_total), &bytes_read,
                  nullptr))
      return false;
    if (bytes_read == 0)
      return false;
    read_total += bytes_read;
    if (read_total < size && !wait_for_data(timeout))
      return false;
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
  if (!read_all(header_buf, header_size, timeout)) {
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
  if (type_byte < 0x01 || type_byte > 0x0C) {
    connected_ = false;
    return std::nullopt;
  }

  WireFrame frame;
  frame.type = static_cast<MessageType>(type_byte);
  if (payload_len > 0) {
    frame.payload.resize(payload_len);
    if (!read_all(frame.payload.data(), payload_len, timeout)) {
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
