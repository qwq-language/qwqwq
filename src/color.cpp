#include "color.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace qwq::color {
namespace {

bool outEnabled = false;
bool errEnabled = false;

bool envSet(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && *value != '\0';
}

std::string envValue(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string() : std::string(value);
}

#if defined(_WIN32)
// Windows consoles need ENABLE_VIRTUAL_TERMINAL_PROCESSING before they interpret
// ANSI escapes; older conhost would otherwise print the escape bytes verbatim.
bool enableVirtualTerminal(DWORD handleId) {
  const HANDLE handle = GetStdHandle(handleId);
  if (handle == INVALID_HANDLE_VALUE || handle == nullptr) return false;

  DWORD mode = 0;
  if (!GetConsoleMode(handle, &mode)) return false;
  if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) return true;
  return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}

bool terminalOut() { return _isatty(_fileno(stdout)) != 0 && enableVirtualTerminal(STD_OUTPUT_HANDLE); }
bool terminalErr() { return _isatty(_fileno(stderr)) != 0 && enableVirtualTerminal(STD_ERROR_HANDLE); }
#else
bool terminalOut() { return isatty(fileno(stdout)) != 0; }
bool terminalErr() { return isatty(fileno(stderr)) != 0; }
#endif

bool forcedByEnv() {
  const std::string value = envValue("CLICOLOR_FORCE");
  return !value.empty() && value != "0";
}

}  // namespace

void configure(Mode mode) {
#if defined(__EMSCRIPTEN__)
  // The WASM bridge hands diagnostics to the editor as JSON; escape bytes would
  // corrupt that payload, so the browser build never colours anything.
  (void)mode;
  outEnabled = false;
  errEnabled = false;
#else
  if (mode == Mode::Never) return;
  if (mode == Mode::Always) {
    outEnabled = true;
    errEnabled = true;
    return;
  }

  if (envSet("NO_COLOR")) return;
  if (envValue("TERM") == "dumb") return;
  if (forcedByEnv()) {
    outEnabled = true;
    errEnabled = true;
    return;
  }

  outEnabled = terminalOut();
  errEnabled = terminalErr();
#endif
}

bool forOut() noexcept { return outEnabled; }
bool forErr() noexcept { return errEnabled; }

namespace {

const char* pick(bool toErr, const char* sequence) {
  return (toErr ? errEnabled : outEnabled) ? sequence : "";
}

}  // namespace

const char* reset(bool toErr) noexcept { return pick(toErr, "\033[0m"); }
const char* bold(bool toErr) noexcept { return pick(toErr, "\033[1m"); }
const char* dim(bool toErr) noexcept { return pick(toErr, "\033[2m"); }
const char* red(bool toErr) noexcept { return pick(toErr, "\033[31m"); }
const char* green(bool toErr) noexcept { return pick(toErr, "\033[32m"); }
const char* cyan(bool toErr) noexcept { return pick(toErr, "\033[36m"); }
const char* yellow(bool toErr) noexcept { return pick(toErr, "\033[33m"); }

}  // namespace qwq::color
