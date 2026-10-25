#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "checker.hpp"
#include "color.hpp"
#include "diagnostic.hpp"
#include "error.hpp"
#include "fmt.hpp"
#include "interpreter.hpp"
#include "lexer.hpp"
#include "loader.hpp"
#include "parser.hpp"
#include "repl.hpp"
#include "version.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

struct Options {
  std::string file;
  bool checkOnly = false;
  bool dumpTree = false;
  bool repl = false;
  bool format = false;
  bool formatCheck = false;
  qwq::color::Mode colorMode = qwq::color::Mode::Auto;
  std::int64_t stepLimit = INT64_MAX;
};

void printUsage(std::ostream& out) {
  out << "qwqwq (qwq language interpreter) " << qwq::kVersion << "\n"
      << "\n"
      << "Usage: qwqwq [options] [file]\n"
      << "\n"
      << "Without a file, reads the program from standard input.\n"
      << "If standard input is a terminal, starts the REPL instead.\n"
      << "\n"
      << "Options:\n"
      << "  -               read the program from standard input\n"
      << "  --check         type check only\n"
      << "  --dump          print the syntax tree with inferred types\n"
      << "  --fmt           rewrite the source in canonical layout\n"
      << "  --fmt-check     exit 1 when the source is not in canonical layout\n"
      << "  --repl          start the REPL even when standard input is not a terminal\n"
      << "  --max-steps N   evaluation step limit\n"
      << "  --color WHEN    colour output: auto (default), always, never\n"
      << "  -h, --help      show this help\n"
      << "  -v, --version   show the version\n"
      << "\n"
      << "A line that starts with '=>' reads a header.\n"
      << "Headers are looked up next to this file, then in $QWQ_PATH, the installation's awa/ directory.\n";
}

/// Where `=> io.awa` is looked up: `$QWQ_PATH`, then `awa/` beside the executable, then `awa/` in the working directory.
std::vector<std::string> libraryPaths() {
  std::vector<std::string> paths;
  const auto split = [&](const std::string& value, char separator) {
    std::size_t start = 0;
    while (start <= value.size()) {
      const std::size_t end = value.find(separator, start);
      const std::string part = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (!part.empty()) paths.push_back(part);
      if (end == std::string::npos) break;
      start = end + 1;
    }
  };

  if (const char* configured = std::getenv("QWQ_PATH"); configured != nullptr) {
    split(configured, ':');
#if defined(_WIN32)
    split(configured, ';');
#endif
  }

#if defined(_WIN32)
  char executable[1024] = {};
  const DWORD length = GetModuleFileNameA(nullptr, executable, sizeof(executable));
  if (length > 0 && length < sizeof(executable)) {
    std::filesystem::path path(executable);
    paths.push_back((path.parent_path() / "awa").string());
  }
#else
  char executable[1024] = {};
  const ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
  if (length > 0) {
    std::filesystem::path path(std::string(executable, static_cast<std::size_t>(length)));
    paths.push_back((path.parent_path() / "awa").string());
  }
#endif

  paths.emplace_back("awa");
  return paths;
}

bool stdinIsTerminal() {
#if defined(_WIN32)
  return _isatty(_fileno(stdin)) != 0;
#else
  return isatty(fileno(stdin)) != 0;
#endif
}

bool parseOptions(int argc, char** argv, Options& options, int& exitCode) {
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];

    if (argument == "-h" || argument == "--help") {
      printUsage(std::cout);
      exitCode = 0;
      return false;
    }
    if (argument == "-v" || argument == "--version") {
      std::cout << qwq::color::dim(false) << "qwqwq (qwq language interpreter)" << qwq::color::reset(false)
                << ' ' << qwq::color::bold(false) << qwq::color::green(false) << qwq::kVersion
                << qwq::color::reset(false) << "\n";
      exitCode = 0;
      return false;
    }
    if (argument == "--check") {
      options.checkOnly = true;
      continue;
    }
    if (argument == "--dump") {
      options.dumpTree = true;
      continue;
    }
    if (argument == "--repl") {
      options.repl = true;
      continue;
    }
    if (argument == "--fmt") {
      options.format = true;
      continue;
    }
    if (argument == "--fmt-check") {
      options.formatCheck = true;
      continue;
    }
    if (argument == "--max-steps") {
      if (i + 1 >= argc) {
        std::cerr << qwq::color::bold(true) << qwq::color::red(true) << "error:" << qwq::color::reset(true)
                << " --max-steps requires a number\n";
        exitCode = 2;
        return false;
      }
      try {
        options.stepLimit = std::stoll(argv[++i]);
      } catch (const std::exception&) {
        std::cerr << qwq::color::bold(true) << qwq::color::red(true) << "error:" << qwq::color::reset(true)
                << " --max-steps requires a number\n";
        exitCode = 2;
        return false;
      }
      continue;
    }
    if (argument == "--color" || argument.rfind("--color=", 0) == 0) {
      std::string value = argument == "--color" ? (i + 1 < argc ? argv[++i] : "") : argument.substr(8);
      if (value == "auto") options.colorMode = qwq::color::Mode::Auto;
      else if (value == "always") options.colorMode = qwq::color::Mode::Always;
      else if (value == "never") options.colorMode = qwq::color::Mode::Never;
      else {
        std::cerr << "error: --color expects auto, always or never\n";
        exitCode = 2;
        return false;
      }
      continue;
    }
    if (argument == "--no-color") {
      options.colorMode = qwq::color::Mode::Never;
      continue;
    }
    if (!argument.empty() && argument[0] == '-' && argument != "-") {
      std::cerr << qwq::color::bold(true) << qwq::color::red(true) << "error:" << qwq::color::reset(true)
                << " unknown option '" << argument << "'  (try --help)\n";
      exitCode = 2;
      return false;
    }

    options.file = argument;
  }
  return true;
}

std::string readAll(std::istream& in) {
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

}  // namespace

/// Resolves the colour mode before anything is printed, so --version, --help and
/// option errors honour --color/--no-color regardless of where the flag appears.
qwq::color::Mode scanColorMode(int argc, char** argv) {
  qwq::color::Mode mode = qwq::color::Mode::Auto;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--no-color") {
      mode = qwq::color::Mode::Never;
      continue;
    }
    std::string value;
    if (argument == "--color" && i + 1 < argc) value = argv[++i];
    else if (argument.rfind("--color=", 0) == 0) value = argument.substr(8);
    else continue;

    if (value == "always") mode = qwq::color::Mode::Always;
    else if (value == "never") mode = qwq::color::Mode::Never;
    else mode = qwq::color::Mode::Auto;
  }
  return mode;
}

int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);

  qwq::color::configure(scanColorMode(argc, argv));

  Options options;
  int exitCode = 0;
  if (!parseOptions(argc, argv, options, exitCode)) return exitCode;

  const bool inputIsTerminal = stdinIsTerminal();
  const bool useRepl = options.repl || (options.file.empty() && inputIsTerminal);
  if (useRepl) {
    return qwq::runRepl(std::cin, std::cout, std::cerr, options.stepLimit,
                        inputIsTerminal, libraryPaths());
  }

  std::string source;
  if (options.file.empty() || options.file == "-") {
    source = readAll(std::cin);
  } else {
    std::ifstream file(options.file, std::ios::binary);
    if (!file) {
      std::cerr << qwq::color::bold(true) << qwq::color::red(true) << "error:" << qwq::color::reset(true)
                << " cannot open '" << options.file << "'\n";
      return 2;
    }
    source = readAll(file);
  }

  try {
    if (options.format || options.formatCheck) {
      const bool header = options.file.size() > 4 &&
                          options.file.compare(options.file.size() - 4, 4, ".awa") == 0;
      const std::string formatted = qwq::format(source, header);
      if (options.format) {
        std::cout << formatted;
        return 0;
      }
      if (formatted == source) return 0;
      std::size_t line = 1;
      for (std::size_t i = 0; i < source.size() && i < formatted.size(); ++i) {
        if (source[i] != formatted[i]) break;
        if (source[i] == '\n') ++line;
      }
      throw qwq::Error(
          "the source is not in canonical layout (first difference around line " +
              std::to_string(line) + "); run `qwqwq --fmt` to rewrite it",
          0);
    }

    const qwq::FileSystemProvider provider;
    const std::vector<std::string> searchPaths = libraryPaths();
    const std::string path =
        options.file.empty() || options.file == "-" ? "<stdin>" : options.file;
    qwq::Program program =
        options.file.empty() || options.file == "-"
            ? qwq::loadSource(source, path, provider, searchPaths)
            : qwq::loadProgram(path, provider, searchPaths);
    qwq::check(program);

    if (!program.main) {
      if (options.dumpTree) {
        qwq::dump(program, std::cout);
        return 0;
      }
      if (options.checkOnly) {
        if (program.typeAliases.empty()) {
          std::cout << qwq::color::green(false) << "ok:" << qwq::color::reset(false) << ' '
                    << program.definitions.size() << " definition(s)\n";
        } else {
          std::cout << qwq::color::green(false) << "ok:" << qwq::color::reset(false) << ' '
                    << program.typeAliases.size() << " class type(s), "
                    << program.definitions.size() << " definition(s)\n";
        }
      }
      return 0;
    }

    qwq::Expr& main = *program.main;

    if (!main.in.empty()) {
      throw qwq::Error(
          "program is not closed: it consumes a stream of type " +
              qwq::toString(main.in) +
              ", but a program must start from the empty stream o "
              "(every input has to be produced by the program itself)",
          main.position);
    }

    // Output is a library call, so a program is an effect: it must leave
    // nothing on the stream.
    if (!main.out.empty()) {
      throw qwq::Error(
          "program must end with the empty stream o, but it ends with " +
              qwq::toString(main.out) +
              "; print through the library, e.g. `... ~ print`",
          main.position);
    }

    if (options.dumpTree) {
      qwq::dump(program, std::cout);
      return 0;
    }

    if (options.checkOnly) {
      std::cout << qwq::color::green(false) << "ok:" << qwq::color::reset(false) << ' '
                << qwq::toString(main.in) << " -> "
                << qwq::toString(main.out) << "\n";
      return 0;
    }

    qwq::Interpreter interpreter(std::cout, std::cin, options.stepLimit,
                                 program.definitions, nullptr,
                                 &program.specializations, &std::cerr);
    for (const qwq::Value& value : interpreter.run(main, {})) {
      std::cout << value;
    }
    std::cout.flush();
    return 0;
  } catch (const qwq::Error& error) {
    qwq::reportError(std::cerr, source, error);
    return 1;
  } catch (const std::exception& error) {
    std::cerr << qwq::color::bold(true) << qwq::color::red(true) << "error:" << qwq::color::reset(true)
              << ' ' << error.what() << "\n";
    return 1;
  }
}
