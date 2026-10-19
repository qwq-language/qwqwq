#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "checker.hpp"
#include "diagnostic.hpp"
#include "error.hpp"
#include "fmt.hpp"
#include "interpreter.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "repl.hpp"
#include "version.hpp"

#if defined(_WIN32)
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
      << "  -h, --help      show this help\n"
      << "  -v, --version   show the version\n";
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
      std::cout << "qwqwq (qwq language interpreter) " << qwq::kVersion << "\n";
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
        std::cerr << "error: --max-steps requires a number\n";
        exitCode = 2;
        return false;
      }
      try {
        options.stepLimit = std::stoll(argv[++i]);
      } catch (const std::exception&) {
        std::cerr << "error: --max-steps requires a number\n";
        exitCode = 2;
        return false;
      }
      continue;
    }
    if (!argument.empty() && argument[0] == '-' && argument != "-") {
      std::cerr << "error: unknown option '" << argument << "'  (try --help)\n";
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

int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);

  Options options;
  int exitCode = 0;
  if (!parseOptions(argc, argv, options, exitCode)) return exitCode;

  const bool useRepl = options.repl || (options.file.empty() && stdinIsTerminal());
  if (useRepl) {
    return qwq::runRepl(std::cin, std::cout, std::cerr, options.stepLimit);
  }

  std::string source;
  if (options.file.empty() || options.file == "-") {
    source = readAll(std::cin);
  } else {
    std::ifstream file(options.file, std::ios::binary);
    if (!file) {
      std::cerr << "error: cannot open '" << options.file << "'\n";
      return 2;
    }
    source = readAll(file);
  }

  try {
    if (options.format || options.formatCheck) {
      const std::string formatted = qwq::format(source);
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

    qwq::Program program = qwq::parse(qwq::tokenize(source));
    qwq::check(program);

    if (!program.main) {
      if (options.checkOnly) {
        std::cout << "ok: " << program.definitions.size() << " definition(s)\n";
      }
      return 0;
    }

    qwq::Expr& main = *program.main;

    if (!main.in.empty()) {
      throw qwq::Error(
          "program is not closed: it consumes a stream of type " +
              qwq::toString(main.in, main.domainItems) +
              ", but a program must start from the empty stream o "
              "(every input has to be produced by the program itself)",
          main.position);
    }

    if (options.dumpTree) {
      qwq::dump(program, std::cout);
      return 0;
    }

    if (options.checkOnly) {
      std::cout << "ok: " << qwq::toString(main.in, main.domainItems) << " -> "
                << qwq::toString(main.out, main.codomainItems) << "\n";
      return 0;
    }

    qwq::Interpreter interpreter(std::cout, std::cin, options.stepLimit,
                                 program.definitions);
    for (const qwq::Value& value : interpreter.run(main, {})) {
      std::cout << value;
    }
    std::cout.flush();
    return 0;
  } catch (const qwq::Error& error) {
    qwq::reportError(std::cerr, source, error);
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
}
