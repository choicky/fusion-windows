#include "ipc/pipe_server.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

namespace fusion::ipc {

PipeServer::PipeServer(std::wstring pipe_name) : pipe_name_(std::move(pipe_name)) {}

PipeServer::~PipeServer() { shutdown(); }

bool PipeServer::create() {
  handle_ = CreateNamedPipeW(
      pipe_name_.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
      1, static_cast<DWORD>(max_message_payload + header_size),
      static_cast<DWORD>(max_message_payload + header_size), 0, nullptr);
  return handle_ != INVALID_HANDLE_VALUE;
}

bool PipeServer::wait_for_client(std::chrono::milliseconds timeout) {
  if (handle_ == INVALID_HANDLE_VALUE)
    return false;
  OVERLAPPED overlapped{};
  overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!overlapped.hEvent)
    return false;
  BOOL connected = ConnectNamedPipe(handle_, &overlapped);
  if (!connected) {
    auto err = GetLastError();
    if (err == ERROR_IO_PENDING) {
      auto result = WaitForSingleObject(overlapped.hEvent,
                                        static_cast<DWORD>(timeout.count()));
      if (result != WAIT_OBJECT_0) {
        CancelIo(handle_);
        CloseHandle(overlapped.hEvent);
        return false;
      }
      DWORD bytes_transferred = 0;
      if (!GetOverlappedResult(handle_, &overlapped, &bytes_transferred, FALSE)) {
        CloseHandle(overlapped.hEvent);
        return false;
      }
    } else if (err != ERROR_PIPE_CONNECTED) {
      CloseHandle(overlapped.hEvent);
      return false;
    }
  }
  CloseHandle(overlapped.hEvent);
  pipe_handle_ = handle_;
  client_connected_ = true;
  return true;
}

void PipeServer::disconnect_client() {
  if (client_connected_) {
    DisconnectNamedPipe(handle_);
    client_connected_ = false;
    pipe_handle_ = INVALID_HANDLE_VALUE;
  }
}

void PipeServer::shutdown() {
  shutting_down_ = true;
  disconnect_client();
  if (handle_ != INVALID_HANDLE_VALUE) {
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }
}

bool PipeServer::client_connected() const { return client_connected_; }

bool PipeServer::write_all(const void *data, std::size_t size) {
  const auto *ptr = static_cast<const std::uint8_t *>(data);
  std::size_t written_total = 0;
  while (written_total < size) {
    DWORD written = 0;
    if (!WriteFile(pipe_handle_, ptr + written_total,
                   static_cast<DWORD>(size - written_total), &written, nullptr))
      return false;
    written_total += written;
  }
  return true;
}

bool PipeServer::wait_for_data(std::chrono::milliseconds timeout) {
  DWORD bytes_available = 0;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    if (!PeekNamedPipe(pipe_handle_, nullptr, 0, nullptr, &bytes_available, nullptr))
      return false;
    if (bytes_available > 0)
      return true;
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    Sleep(1);
  }
}

bool PipeServer::read_all(void *data, std::size_t size, std::chrono::milliseconds timeout) {
  if (!wait_for_data(timeout))
    return false;
  auto *ptr = static_cast<std::uint8_t *>(data);
  std::size_t read_total = 0;
  while (read_total < size) {
    DWORD bytes_read = 0;
    if (!ReadFile(pipe_handle_, ptr + read_total, static_cast<DWORD>(size - read_total),
                  &bytes_read, nullptr))
      return false;
    if (bytes_read == 0)
      return false;
    read_total += bytes_read;
    if (read_total < size && !wait_for_data(timeout))
      return false;
  }
  return true;
}

bool PipeServer::send_frame(const WireFrame &frame) {
  if (!client_connected_)
    return false;
  auto wire = frame_message(frame.type, frame.payload);
  return write_all(wire.data(), wire.size());
}

std::optional<WireFrame> PipeServer::recv_frame(std::chrono::milliseconds timeout) {
  if (!client_connected_)
    return std::nullopt;

  std::uint8_t header_buf[header_size];
  if (!read_all(header_buf, header_size, timeout))
    return std::nullopt;

  std::uint32_t payload_len = 0;
  std::memcpy(&payload_len, header_buf, 4);
  if (payload_len > max_message_payload)
    return std::nullopt;

  auto type_byte = header_buf[4];
  if (type_byte < 0x01 || type_byte > 0x0C)
    return std::nullopt;

  WireFrame frame;
  frame.type = static_cast<MessageType>(type_byte);
  if (payload_len > 0) {
    frame.payload.resize(payload_len);
    if (!read_all(frame.payload.data(), payload_len, timeout))
      return std::nullopt;
  }
  return frame;
}

} // namespace fusion::ipc
