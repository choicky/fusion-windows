#include "tsf/diag.hpp"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

std::string path() {
  char temp[MAX_PATH]{};
  require(GetTempPathA(MAX_PATH, temp) != 0, "GetTempPath failed");
  return std::string(temp) + "fusion-tsf-diag-policy.log";
}
}

int main(int argc, char **argv) {
  try {
    require(argc == 2, "mode required");
    const std::string file = path();
    DeleteFileA(file.c_str());
    SetEnvironmentVariableA("FUSION_TSF_LOG", file.c_str());
    SetEnvironmentVariableA("FUSION_TSF_DIAGNOSTICS", argc == 2 && std::string(argv[1]) == "optin"
                                                            ? "1"
                                                            : nullptr);
    fusion::tsf::diag::event("TEST_KEY", "vk=0x41 text=secret");
    if (std::string(argv[1]) == "default") {
      require(GetFileAttributesA(file.c_str()) == INVALID_FILE_ATTRIBUTES,
              "diagnostics are enabled by default");
      return 0;
    }
    fusion::tsf::diag::event("TECHNICAL", "hr=0x0 count=2");
    std::ifstream input(file);
    const std::string contents((std::istreambuf_iterator<char>(input)), {});
    require(contents.find("TEST_KEY privacy=redacted") != std::string::npos,
            "prohibited fields were not redacted");
    require(contents.find("TECHNICAL hr=0x0 count=2") != std::string::npos,
            "technical opt-in event missing");
    require(contents.find("vk=") == std::string::npos && contents.find("text=") == std::string::npos,
            "diagnostic content policy leaked key/text data");
    DeleteFileA(file.c_str());
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
  }
}
