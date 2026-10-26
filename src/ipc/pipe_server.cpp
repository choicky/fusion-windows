#include "ipc/pipe_server.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>

namespace fusion::ipc {

PipeServer::PipeServer(std::wstring pipe_name, bool first_instance)
    : pipe_name_(std::move(pipe_name)), first_instance_(first_instance) {}

PipeServer::~PipeServer() {
  disconnect_client();
  if (handle_ != INVALID_HANDLE_VALUE) {
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }
  free_pipe_security(security_attrs_);
}

bool PipeServer::create() {
  last_error_ = 0;
  // The data pipe is the one object a packaged client has to reach (R49); the
  // claim and stop events keep the user-only descriptor.
  security_attrs_ = make_data_pipe_security();
  if (!security_attrs_.lpSecurityDescriptor)
    return false; // fail closed: security descriptor construction failed
  DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
  if (first_instance_)
    open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
  DWORD pipe_mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
  handle_ =
      CreateNamedPipeW(pipe_name_.c_str(), open_mode, pipe_mode, PIPE_UNLIMITED_INSTANCES,
                       static_cast<DWORD>(max_message_payload + header_size),
                       static_cast<DWORD>(max_message_payload + header_size), 0, &security_attrs_);
  if (handle_ == INVALID_HANDLE_VALUE) {
    last_error_ = GetLastError();
  }
  return handle_ != INVALID_HANDLE_VALUE;
}

bool PipeServer::wait_for_client(std::chrono::milliseconds timeout) {
  last_error_ = 0;
  if (handle_ == INVALID_HANDLE_VALUE) {
    last_error_ = ERROR_INVALID_HANDLE;
    return false;
  }
  if (client_connected_)
    return true;

  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ov.hEvent) {
    last_error_ = GetLastError();
    return false;
  }

  BOOL connected = ConnectNamedPipe(handle_, &ov);
  if (!connected) {
    auto err = GetLastError();
    if (err == ERROR_IO_PENDING) {
      DWORD wait_ms = (timeout.count() == 0) ? INFINITE : static_cast<DWORD>(timeout.count());
      auto result = WaitForSingleObject(ov.hEvent, wait_ms);
      if (result != WAIT_OBJECT_0) {
        CancelIo(handle_);
        DWORD bytes_transferred_cancel = 0;
        GetOverlappedResult(handle_, &ov, &bytes_transferred_cancel, TRUE);
        DisconnectNamedPipe(handle_);
        CloseHandle(ov.hEvent);
        last_error_ = ERROR_TIMEOUT;
        return false;
      }
      DWORD bytes_transferred = 0;
      if (!GetOverlappedResult(handle_, &ov, &bytes_transferred, FALSE)) {
        DisconnectNamedPipe(handle_);
        CloseHandle(ov.hEvent);
        last_error_ = GetLastError();
        return false;
      }
    } else if (err != ERROR_PIPE_CONNECTED) {
      CloseHandle(ov.hEvent);
      last_error_ = err;
      return false;
    }
  }
  CloseHandle(ov.hEvent);
  client_connected_ = true;
  return true;
}

void PipeServer::disconnect_client() {
  if (handle_ == INVALID_HANDLE_VALUE)
    return;
  // CancelIo only cancels I/O issued by the calling thread, so it cannot unblock
  // a receive that another thread (HostServer::stop()) has to interrupt.
  CancelIoEx(handle_, nullptr);
  DisconnectNamedPipe(handle_);
  client_connected_ = false;
}

bool PipeServer::client_connected() const { return client_connected_; }

bool PipeServer::write_all(const void *data, std::size_t size) {
  last_error_ = 0;
  const auto *ptr = static_cast<const std::uint8_t *>(data);
  std::size_t written_total = 0;
  while (written_total < size) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
      last_error_ = GetLastError();
      return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(handle_, ptr + written_total, static_cast<DWORD>(size - written_total),
                        &written, &ov);
    if (ok) {
      CloseHandle(ov.hEvent);
      written_total += written;
      continue;
    }
    auto err = GetLastError();
    if (err != ERROR_IO_PENDING) {
      CloseHandle(ov.hEvent);
      last_error_ = err;
      return false;
    }
    auto result = WaitForSingleObject(ov.hEvent, INFINITE);
    if (result != WAIT_OBJECT_0) {
      CancelIoEx(handle_, &ov);
      GetOverlappedResult(handle_, &ov, &written, TRUE);
      CloseHandle(ov.hEvent);
      last_error_ = ERROR_TIMEOUT;
      return false;
    }
    if (!GetOverlappedResult(handle_, &ov, &written, FALSE)) {
      CloseHandle(ov.hEvent);
      last_error_ = GetLastError();
      return false;
    }
    CloseHandle(ov.hEvent);
    written_total += written;
  }
  return true;
}

bool PipeServer::read_all_overlapped(void *data, std::size_t size,
                                     std::chrono::milliseconds timeout) {
  last_error_ = 0;
  auto *ptr = static_cast<std::uint8_t *>(data);
  std::size_t read_total = 0;
  while (read_total < size) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
      last_error_ = GetLastError();
      return false;
    }
    DWORD bytes_read = 0;
    BOOL ok = ReadFile(handle_, ptr + read_total, static_cast<DWORD>(size - read_total),
                       &bytes_read, &ov);
    if (ok) {
      CloseHandle(ov.hEvent);
      if (bytes_read == 0) {
        last_error_ = ERROR_BROKEN_PIPE;
        return false;
      }
      read_total += bytes_read;
      continue;
    }
    auto err = GetLastError();
    if (err != ERROR_IO_PENDING) {
      CloseHandle(ov.hEvent);
      last_error_ = err;
      return false;
    }
    DWORD wait_ms = (timeout.count() == 0) ? INFINITE : static_cast<DWORD>(timeout.count());
    auto result = WaitForSingleObject(ov.hEvent, wait_ms);
    if (result != WAIT_OBJECT_0) {
      CancelIoEx(handle_, &ov);
      GetOverlappedResult(handle_, &ov, &bytes_read, TRUE);
      CloseHandle(ov.hEvent);
      last_error_ = ERROR_TIMEOUT;
      return false;
    }
    if (!GetOverlappedResult(handle_, &ov, &bytes_read, FALSE)) {
      CloseHandle(ov.hEvent);
      last_error_ = GetLastError();
      return false;
    }
    CloseHandle(ov.hEvent);
    if (bytes_read == 0) {
      last_error_ = ERROR_BROKEN_PIPE;
      return false;
    }
    read_total += bytes_read;
  }
  return true;
}

bool PipeServer::send_frame(const WireFrame &frame) {
  if (!client_connected_) {
    last_error_ = ERROR_PIPE_NOT_CONNECTED;
    return false;
  }
  auto wire = frame_message(frame.type, frame.payload);
  if (!write_all(wire.data(), wire.size())) {
    disconnect_client();
    return false;
  }
  return true;
}

std::optional<WireFrame> PipeServer::recv_frame(std::chrono::milliseconds timeout) {
  if (!client_connected_) {
    last_error_ = ERROR_PIPE_NOT_CONNECTED;
    return std::nullopt;
  }

  std::uint8_t header_buf[header_size];
  if (!read_all_overlapped(header_buf, header_size, timeout))
    return std::nullopt;

  std::uint32_t payload_len = 0;
  std::memcpy(&payload_len, header_buf, 4);
  if (payload_len > max_message_payload) {
    last_error_ = ERROR_INVALID_DATA;
    return std::nullopt;
  }

  auto type_byte = header_buf[4];
  if (type_byte < 0x01 || type_byte > max_message_type) {
    last_error_ = ERROR_INVALID_DATA;
    return std::nullopt;
  }

  WireFrame frame;
  frame.type = static_cast<MessageType>(type_byte);
  if (payload_len > 0) {
    frame.payload.resize(payload_len);
    if (!read_all_overlapped(frame.payload.data(), payload_len, timeout))
      return std::nullopt;
  }
  return frame;
}

} // namespace fusion::ipc
