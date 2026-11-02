// R53 measurement: a long-run soak of the product Host on a throwaway profile.
//
// Not a test and not a gate: it runs FusionHost.exe with its own pipe, its own Fcitx
// configuration and data directories, and several client connections typing at once, and it
// records the series the reviewer asked for — memory, handles, threads, per-key round-trip
// latency, and what the engine's own user-data writes did to that latency.
//
// It never touches %APPDATA%\Fcitx5, and that costs the soak one of the series it was meant to
// record. Measured: this runtime does not honour FCITX_CONFIG_HOME/FCITX_DATA_HOME on Windows —
// a run with both pointing at a throwaway directory still wrote its learning data into the real
// %APPDATA%\Fcitx5 (2026-11-02, recorded in the R53 report). The only safe isolation this
// platform offers is FUSION_FCITX_SKIP_USER_PATH=1, which is what this harness sets: the engine
// then persists nothing, so the autosave / user-data-write series cannot be measured here and
// the soak reports zeros for it rather than guessing.
//
// usage: soak <minutes> <clients> <outdir> [pipe-suffix]

#include "fusion/backend.hpp"
#include "fusion/ipc_protocol.hpp"
#include "fusion/settings_protocol.hpp"
#include "ipc/ipc_backend.hpp"
#include "ipc/pipe_client.hpp"
#include "tsf/key_map.hpp"
#include "tsf/semantic.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace fusion;
using namespace fusion::tsf;

namespace {

constexpr std::chrono::milliseconds kWait{10000};
constexpr std::chrono::milliseconds kSamplePeriod{10 * 60 * 1000}; // every 10 minutes
constexpr std::chrono::milliseconds kWriteWindow{2000};            // "during a save"

std::wstring executable_directory() {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH)
    return {};
  const std::wstring value(path, length);
  const auto slash = value.find_last_of(L'\\');
  return slash == std::wstring::npos ? std::wstring() : value.substr(0, slash);
}

std::wstring pipe_name(const wchar_t *suffix) {
  return std::wstring(L"\\\\.\\pipe\\FusionHost-soak-") + suffix;
}

bool wait_for_host_pipe(const std::wstring &name, std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (WaitNamedPipeW(name.c_str(), 1))
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return WaitNamedPipeW(name.c_str(), 1) != FALSE;
}

bool connect_with_retry(ipc::IpcBackend &client, std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (client.ensure_link(std::chrono::milliseconds(200)))
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return client.ensure_link(std::chrono::milliseconds(200));
}

class HostProcess {
public:
  HostProcess(std::wstring executable, std::wstring pipe, std::wstring log)
      : executable_(std::move(executable)), pipe_(std::move(pipe)), log_(std::move(log)) {}
  ~HostProcess() {
    kill();
    if (log_file_ != INVALID_HANDLE_VALUE)
      CloseHandle(log_file_);
  }

  bool start() {
    kill();
    // The Host's own stderr is the only place it says which Fcitx directories it resolved, so
    // the soak keeps it: that line is the proof the throwaway profile was used and the owner's
    // own data was not.
    if (log_file_ == INVALID_HANDLE_VALUE) {
      log_file_ = CreateFileW(log_.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    std::wstring command = L"\"" + executable_ + L"\" --pipe-name " + pipe_;
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = log_file_;
    startup.hStdError = log_file_;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable_.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
      return false;
    CloseHandle(info.hThread);
    process_ = info.hProcess;
    return true;
  }

  bool running() const {
    if (process_ == nullptr)
      return false;
    DWORD code = 0;
    return GetExitCodeProcess(process_, &code) && code == STILL_ACTIVE;
  }

  void kill() {
    if (process_ == nullptr)
      return;
    if (running()) {
      TerminateProcess(process_, 1);
      WaitForSingleObject(process_, 10000);
    }
    CloseHandle(process_);
    process_ = nullptr;
  }

  struct Metrics {
    unsigned long long private_bytes = 0;
    unsigned long handles = 0;
    unsigned long threads = 0;
    bool valid = false;
  };

  Metrics sample() const {
    Metrics metrics;
    if (process_ == nullptr || !running())
      return metrics;
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(process_, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                             sizeof(counters)))
      metrics.private_bytes = counters.PrivateUsage;
    DWORD handles = 0;
    if (GetProcessHandleCount(process_, &handles))
      metrics.handles = handles;
    const DWORD pid = GetProcessId(process_);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
      THREADENTRY32 entry{};
      entry.dwSize = sizeof(entry);
      if (Thread32First(snapshot, &entry)) {
        do {
          if (entry.th32OwnerProcessID == pid)
            ++metrics.threads;
        } while (Thread32Next(snapshot, &entry));
      }
      CloseHandle(snapshot);
    }
    metrics.valid = true;
    return metrics;
  }

private:
  std::wstring executable_;
  std::wstring pipe_;
  std::wstring log_;
  HANDLE log_file_ = INVALID_HANDLE_VALUE;
  HANDLE process_ = nullptr;
};

// One key's round-trip cost, with the wall clock it happened at (ms since the soak started).
struct Sample {
  std::uint64_t at_ms = 0;
  double micros = 0;
};

std::mutex samples_mutex;
std::vector<Sample> samples;

std::uint64_t g_start = 0;
std::uint64_t now_ms() {
  return static_cast<std::uint64_t>(GetTickCount64()) - g_start;
}

double percentile(std::vector<double> values, double fraction) {
  if (values.empty())
    return 0.0;
  std::sort(values.begin(), values.end());
  const std::size_t index = static_cast<std::size_t>(fraction * (values.size() - 1));
  return values[index];
}

// The profile directory the Host writes: file count, total bytes and the newest write time, so
// the sampler can tell whether a save happened inside the window it is reporting.
struct ProfileState {
  std::size_t files = 0;
  unsigned long long bytes = 0;
  unsigned long long newest_write_ms = 0;
  bool valid = false;
};

ProfileState scan_profile(const std::wstring &root) {
  ProfileState state;
  WIN32_FIND_DATAW data{};
  const std::wstring pattern = root + L"\\*";
  HANDLE find = FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE)
    return state;
  state.valid = true;
  do {
    const std::wstring name = data.cFileName;
    if (name == L"." || name == L"..")
      continue;
    const std::wstring full = root + L"\\" + name;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      const ProfileState child = scan_profile(full);
      state.files += child.files;
      state.bytes += child.bytes;
      if (child.newest_write_ms > state.newest_write_ms)
        state.newest_write_ms = child.newest_write_ms;
      continue;
    }
    ++state.files;
    state.bytes += (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    const ULARGE_INTEGER stamp{data.ftLastWriteTime.dwLowDateTime,
                               data.ftLastWriteTime.dwHighDateTime};
    const unsigned long long ms = stamp.QuadPart / 10000ull; // 100 ns since 1601 -> ms
    if (ms > state.newest_write_ms)
      state.newest_write_ms = ms;
  } while (FindNextFileW(find, &data));
  FindClose(find);
  return state;
}

// A settings read on its own connection: the Host serves one settings request per connection
// after a settings hello, and this is the read the settings window performs when it opens.
bool settings_read(const std::wstring &pipe) {
  ipc::PipeClient client(pipe);
  if (!client.connect(std::chrono::milliseconds(500)))
    return false;
  const auto hello = ipc::serialize_settings_hello(ipc::protocol_version,
                                                   ipc::settings_api_version);
  if (client.send_request(ipc::MessageType::SettingsHelloRequest, hello) !=
      ipc::WriteOutcome::Written)
    return false;
  if (!client.recv_response(std::chrono::milliseconds(2000)))
    return false;
  if (client.send_request(ipc::MessageType::SettingsGetSelectionRequest, {}) !=
      ipc::WriteOutcome::Written)
    return false;
  return client.recv_response(std::chrono::milliseconds(2000)).has_value();
}

// One client connection: its own backend, its own context, its own typing script.
class Client {
public:
  Client(std::wstring pipe, std::uint64_t context, std::uint32_t seed)
      : backend_(std::move(pipe)), context_(context), random_(seed) {}

  bool start() {
    if (!connect_with_retry(backend_, kWait))
      return false;
    controller_ = std::make_unique<SemanticController>(context_, 128, backend_, 2);
    return controller_->semantic_ready();
  }

  // Types one key and returns its round-trip cost in microseconds. The script is a realistic
  // mix rather than one repeated key: a word committed, a word abandoned, English letters,
  // punctuation, a candidate selection by number, and a page turn.
  double type_one_key() {
    const int step = static_cast<int>(step_++ % 40);
    KeyAction action = next_action(step);
    const auto began = std::chrono::steady_clock::now();
    const SemanticDecision decision = controller_->begin_key(action, true);
    settle(decision);
    const auto ended = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::micro>(ended - began).count();
  }

  bool healthy() const { return controller_ != nullptr && controller_->semantic_ready(); }
  void stop() {
    if (controller_)
      controller_->destroy();
    controller_.reset();
  }

private:
  KeyAction next_action(int step) {
    // A word of pinyin, then Space, then a pause-like Escape; letters are the pinyin ones.
    static const char *const syllables[] = {"ni", "hao", "zhong", "wen", "shi", "jie"};
    if (step % 10 == 9) {
      // Every tenth key commits what is composing, or is a space when nothing is.
      return translate_key(kVkSpace, false, false, false);
    }
    if (step % 10 == 8) {
      // A candidate selection by number while a page is up; otherwise a digit that the engine
      // forwards, which is the digit path too.
      return translate_key('1' + (random_() % 3), false, false, false, '1');
    }
    if (step % 17 == 5)
      return translate_key(0xBC, false, false, false, U','); // punctuation
    if (step % 23 == 7)
      return translate_key(0xBE, false, false, false, U'.');
    if (step % 29 == 11) {
      // English: a letter typed while the engine is off is delivered by the fallback, and one
      // typed while it is on composes — both are the product's own behaviour, not a fault.
      const char letter = static_cast<char>('a' + (random_() % 26));
      return translate_key(static_cast<std::uint32_t>(letter), false, false, false, letter);
    }
    const std::string syllable = syllables[random_() % (sizeof(syllables) / sizeof(syllables[0]))];
    const char letter = syllable[random_() % syllable.size()];
    return translate_key(static_cast<std::uint32_t>(letter), false, false, false, letter);
  }

  void settle(const SemanticDecision &decision) {
    for (const auto &work : decision.frontend) {
      if (work.operation.type == BackendOperation::Type::CommitText) {
        auto ticket = controller_->open_effect(work.key);
        if (ticket)
          (void)controller_->settle_effect(*ticket, EditResult::AppliedReadback);
      } else if (work.operation.type == BackendOperation::Type::SetPreedit) {
        (void)controller_->settle_state(work.key, EditResult::AppliedReadback);
      }
    }
  }

  ipc::IpcBackend backend_;
  std::unique_ptr<SemanticController> controller_;
  std::uint64_t context_ = 0;
  std::uint64_t step_ = 0;
  std::mt19937 random_;
};

} // namespace

int main(int argc, char **argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: soak <minutes> <clients> <outdir> [pipe-suffix]\n");
    return 2;
  }
  const int minutes = std::atoi(argv[1]);
  const int clients = std::atoi(argv[2]);
  const std::string out_dir = argv[3];
  const std::wstring suffix = argc > 4 ? std::wstring(argv[4], argv[4] + std::strlen(argv[4]))
                                       : L"main";
  if (minutes <= 0 || clients <= 0)
    return 2;

  // No user data: the only isolation this runtime offers (see the header). The directory is
  // still created and still scanned, so a run that unexpectedly writes anything is visible in
  // the CSV instead of silently changing the owner's profile.
  const std::string profile = out_dir + "\\profile";
  CreateDirectoryA(out_dir.c_str(), nullptr);
  CreateDirectoryA(profile.c_str(), nullptr);
  const std::wstring profile_w(profile.begin(), profile.end());
  SetEnvironmentVariableW(L"FUSION_FCITX_SKIP_USER_PATH", L"1");

  const std::wstring executable = executable_directory() + L"\\FusionHost.exe";
  if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::fprintf(stderr, "FAIL FusionHost.exe is not next to the soak binary\n");
    return 1;
  }
  const auto name = pipe_name(suffix.c_str());
  const std::wstring host_log(out_dir.begin(), out_dir.end());
  HostProcess host(executable, name, host_log + L"\\host.log");
  if (!host.start()) {
    std::fprintf(stderr, "FAIL the Host did not start\n");
    return 1;
  }
  if (!wait_for_host_pipe(name, kWait)) {
    std::fprintf(stderr, "FAIL the Host did not listen\n");
    return 1;
  }

  std::vector<std::unique_ptr<Client>> connections;
  for (int i = 0; i < clients; ++i) {
    auto client = std::make_unique<Client>(name, 3000 + static_cast<std::uint64_t>(i), 17u + i);
    if (!client->start()) {
      std::fprintf(stderr, "FAIL client %d did not become ready\n", i);
      return 1;
    }
    connections.push_back(std::move(client));
  }
  std::printf("soak: %d clients connected, %d minutes\n", clients, minutes);

  g_start = GetTickCount64();
  std::atomic<bool> stop{false};
  std::vector<std::thread> typists;
  for (int i = 0; i < clients; ++i) {
    typists.emplace_back([&, i] {
      // A few keys per second each, with a per-client offset so the connections do not march
      // in lock step.
      const auto period = std::chrono::milliseconds(300 + i * 17);
      std::mt19937 jitter(static_cast<std::uint32_t>(i) + 91u);
      while (!stop.load()) {
        if (!connections[static_cast<std::size_t>(i)]->healthy()) {
          // A reconnect is part of the product's behaviour, not a failure of the soak.
          connections[static_cast<std::size_t>(i)]->stop();
          (void)connections[static_cast<std::size_t>(i)]->start();
        }
        const double micros = connections[static_cast<std::size_t>(i)]->type_one_key();
        {
          std::lock_guard<std::mutex> guard(samples_mutex);
          samples.push_back(Sample{now_ms(), micros});
        }
        std::this_thread::sleep_for(period + std::chrono::milliseconds(jitter() % 60));
      }
    });
  }

  const std::string csv_path = out_dir + "\\soak.csv";
  std::ofstream csv(csv_path, std::ios::binary);
  csv << "minute,private_bytes,handles,threads,keys,keys_this_window,p50_us,p99_us,max_us,"
         "profile_files,profile_bytes,profile_written,max_us_near_write,settings_ok\r\n";
  csv.flush();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(minutes);
  int window = 0;
  unsigned long long total_keys = 0;
  ProfileState previous_profile = scan_profile(profile_w);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto next = std::min(deadline, std::chrono::steady_clock::now() + kSamplePeriod);
    std::this_thread::sleep_until(next);
    ++window;
    const HostProcess::Metrics metrics = host.sample();
    std::vector<Sample> window_samples;
    {
      std::lock_guard<std::mutex> guard(samples_mutex);
      window_samples.swap(samples);
    }
    std::vector<double> micros;
    micros.reserve(window_samples.size());
    for (const Sample &sample : window_samples)
      micros.push_back(sample.micros);
    const ProfileState profile_now = scan_profile(profile_w);
    FILETIME wall_filetime{};
    GetSystemTimeAsFileTime(&wall_filetime);
    const unsigned long long wall_now =
        ((static_cast<unsigned long long>(wall_filetime.dwHighDateTime) << 32) |
         wall_filetime.dwLowDateTime) /
        10000ull;
    const bool wrote = profile_now.newest_write_ms != previous_profile.newest_write_ms ||
                       profile_now.files != previous_profile.files ||
                       profile_now.bytes != previous_profile.bytes;
    // Keys that arrived within the write window after the newest write: their latency is the
    // cost of a save that landed while the user was typing.
    double near_write = 0.0;
    if (wrote) {
      const unsigned long long newest = profile_now.newest_write_ms;
      const unsigned long long since = now_ms();
      for (const Sample &sample : window_samples) {
        // `at_ms` is the soak-relative time the key was dispatched; `wall_now` is the wall clock
        // now, so this is the wall clock of that key.
        const unsigned long long at_wall = wall_now - (since - sample.at_ms);
        if (at_wall + static_cast<unsigned long long>(kWriteWindow.count()) >= newest &&
            sample.micros > near_write)
          near_write = sample.micros;
      }
    }
    const bool settings = settings_read(name);
    total_keys += micros.size();
    csv << window * 10 << ',' << metrics.private_bytes << ',' << metrics.handles << ','
        << metrics.threads << ',' << total_keys << ','
        << micros.size() << ',' << percentile(micros, 0.50) << ',' << percentile(micros, 0.99)
        << ',' << percentile(micros, 1.0) << ',' << profile_now.files << ','
        << profile_now.bytes << ',' << (wrote ? 1 : 0) << ',' << near_write << ','
        << (settings ? 1 : 0) << "\r\n";
    csv.flush();
    std::printf("window %d: keys=%zu p50=%.0fus p99=%.0fus max=%.0fus private=%llu handles=%lu "
                "threads=%lu profile_files=%zu wrote=%d settings=%d\n",
                window, micros.size(), percentile(micros, 0.50), percentile(micros, 0.99),
                percentile(micros, 1.0), metrics.private_bytes, metrics.handles, metrics.threads,
                profile_now.files, wrote ? 1 : 0, settings ? 1 : 0);
    std::fflush(stdout);
    previous_profile = profile_now;
  }

  stop.store(true);
  for (auto &thread : typists)
    thread.join();
  for (auto &client : connections)
    client->stop();
  csv.close();
  host.kill();
  std::printf("soak done: %d windows, CSV %s\n", window, csv_path.c_str());
  return 0;
}
