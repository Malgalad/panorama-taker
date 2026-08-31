#include "pano_app.h"

#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>

namespace {
std::string utf8(const wchar_t *value) {
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                       nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(),
                      size, nullptr, nullptr);
  result.pop_back();
  return result;
}
} // namespace

int wmain(const int argc, wchar_t **argv) {
  std::vector<std::string> arguments;
  for (int index = 1; index < argc; ++index) {
    arguments.push_back(utf8(argv[index]));
  }
  return pano::app::run(arguments, std::cout, std::cerr);
}
#else
int main(const int argc, char **argv) {
  return pano::app::run({argv + 1, argv + argc}, std::cout, std::cerr);
}
#endif
