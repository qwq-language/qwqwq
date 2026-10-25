#pragma once

namespace qwq::color {

/// Colour policy for terminal output.
enum class Mode { Auto, Always, Never };

/// Decides whether stdout/stderr may carry ANSI colours. Call once at startup
/// after the command line has been parsed. `Auto` honours TTY state, `NO_COLOR`,
/// `TERM=dumb`, and `CLICOLOR_FORCE`.
void configure(Mode mode);

/// True when the stream may be coloured. WASM builds never colour anything.
bool forOut() noexcept;
bool forErr() noexcept;

/// Escape sequences, empty when colour is disabled for that stream.
const char* reset(bool toErr) noexcept;
const char* bold(bool toErr) noexcept;
const char* dim(bool toErr) noexcept;
const char* red(bool toErr) noexcept;
const char* green(bool toErr) noexcept;
const char* cyan(bool toErr) noexcept;
const char* yellow(bool toErr) noexcept;

}  // namespace qwq::color
