#include "repl.hpp"

#include <iostream>
#include <string>

#include "checker.hpp"
#include "diagnostic.hpp"
#include "error.hpp"
#include "interpreter.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "version.hpp"

namespace qwq {
namespace {

constexpr const char* kPrompt = "qwq> ";
constexpr const char* kContinuation = "...> ";

const char* kHelp =
    "Commands:\n"
    "  :help            show this help\n"
    "  :quit, :q        leave the REPL\n"
    "  :type <expr>     print the type of an expression without running it\n"
    "  :dump <expr>     print the syntax tree with inferred types\n"
    "  :defs            list the functions defined in this session\n"
    "\n"
    "Anything else is parsed as qwq source.\n"
    "Definitions written with `name => body` stay in scope for later lines.\n"
    "Add `name : A -> B => body` when the body calls itself.\n"
    "Expressions whose domain is o are evaluated and their resulting stream is printed.\n";

std::string trim(const std::string& text) {
  const std::size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const std::size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

/// True when the text has an unclosed bracket or an unterminated string, so
/// the REPL should ask for another line.
bool isIncomplete(const std::string& text) {
  int depth = 0;
  bool inString = false;

  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (inString) {
      if (c == '\\') {
        ++i;
      } else if (c == '"') {
        inString = false;
      }
      continue;
    }
    if (c == '"') inString = true;
    else if (c == '(' || c == '[') ++depth;
    else if (c == ')' || c == ']') --depth;
  }
  return inString || depth > 0;
}

/// A line is a definition when it parses as one: `name => body`, or
/// `name : A -> B => body`.  Parsing is the only honest test now that a
/// definition no longer starts with a keyword, and a failed parse is reported
/// by the caller anyway.
bool startsWithDefinition(const std::string& text) {
  try {
    const std::vector<Token> tokens = tokenize(text);
    return !parse(tokens).definitions.empty();
  } catch (const Error&) {
    return false;
  }
}

/// A REPL session: the definitions written so far, plus the machinery to
/// check and run later lines against them.
class Session {
 public:
  Session(std::ostream& output, std::ostream& errors, std::istream& input,
          std::int64_t stepLimit)
      : output_(output), errors_(errors), input_(input), stepLimit_(stepLimit) {}

  /// Validates a definition and keeps it in scope for later lines.
  void define(const std::string& source) {
    const std::string combined = definitions_ + source + "\n";
    Program program = parse(tokenize(combined));
    check(program);

    if (program.definitions.empty()) {
      errors_ << "error: expected a definition\n";
      return;
    }

    definitions_ = combined;
    show(program.definitions.back());  // report only the line just entered
  }

  void listDefinitions() const {
    if (definitions_.empty()) {
      output_ << "no functions defined\n";
      return;
    }
    Program program = parse(tokenize(definitions_));
    check(program);
    for (const Definition& definition : program.definitions) show(definition);
  }

  void typeOf(const std::string& line) {
    const Program program = parseChecked(line);
    const Expr& main = *program.main;
    output_ << toString(main.in, main.domainItems) << " -> "
            << toString(main.out, main.codomainItems) << "\n";
  }

  void dumpOf(const std::string& line) {
    const Program program = parseChecked(line);
    dump(*program.main, output_);
  }

  void evaluate(const std::string& line) {
    const Program program = parseChecked(line);
    const Expr& main = *program.main;

    output_ << "[" << toString(main.in, main.domainItems) << " -> "
            << toString(main.out, main.codomainItems) << "]";

    if (!main.in.empty()) {
      output_ << "  (expects an input stream of type "
              << toString(main.in, main.domainItems)
              << "; not evaluated)\n";
      return;
    }

    Interpreter interpreter(output_, input_, stepLimit_, program.definitions);
    const Stream result = interpreter.run(main, {});
    if (result.empty()) {
      output_ << " => (empty stream)\n";
      return;
    }

    output_ << " => ";
    for (const Value& value : result) output_ << value;
    output_ << "\n";
  }

 private:
  std::ostream& output_;
  std::ostream& errors_;
  std::istream& input_;
  std::int64_t stepLimit_;
  std::string definitions_;

  /// Parses and checks `line` in the scope of the session's definitions.
  Program parseChecked(const std::string& line) {
    Program program = parse(tokenize(definitions_ + line));
    if (!program.main) throw Error("expected an expression");
    check(program);
    return program;
  }

  void show(const Definition& definition) const {
    output_ << definition.name << " : "
            << toString(definition.in, definition.domainItems) << " -> "
            << toString(definition.out, definition.codomainItems) << "\n";
  }
};

/// Handles a `:command` line.  Returns false when the REPL should exit.
bool handleCommand(const std::string& line, Session& session, std::ostream& output,
                   std::ostream& errors) {
  const std::size_t space = line.find_first_of(" \t");
  const std::string command = line.substr(0, space);
  const std::string argument =
      space == std::string::npos ? "" : trim(line.substr(space + 1));

  if (command == ":quit" || command == ":q") return false;

  if (command == ":help") {
    output << kHelp;
    return true;
  }

  if (command == ":defs") {
    session.listDefinitions();
    return true;
  }

  if (command == ":type" || command == ":dump") {
    if (argument.empty()) {
      errors << "error: " << command << " needs an expression\n";
      return true;
    }
    try {
      if (command == ":type") {
        session.typeOf(argument);
      } else {
        session.dumpOf(argument);
      }
    } catch (const Error& error) {
      reportError(errors, argument, error);
    }
    return true;
  }

  errors << "error: unknown command '" << command << "' (try :help)\n";
  return true;
}

}  // namespace

int runRepl(std::istream& input, std::ostream& output, std::ostream& errors,
            std::int64_t stepLimit) {
  output << "qwqwq (qwq language interpreter) " << kVersion << "\n"
         << "Type :help for help, :quit to exit.\n";

  Session session(output, errors, input, stepLimit);
  std::string buffer;

  for (;;) {
    output << (buffer.empty() ? kPrompt : kContinuation) << std::flush;

    std::string line;
    if (!std::getline(input, line)) {
      output << "\n";
      return 0;
    }

    if (buffer.empty()) {
      const std::string trimmed = trim(line);
      if (trimmed.empty()) continue;
      if (trimmed[0] == ':') {
        if (!handleCommand(trimmed, session, output, errors)) return 0;
        continue;
      }
    }

    buffer += line;
    buffer += "\n";

    if (isIncomplete(buffer)) continue;

    try {
      if (startsWithDefinition(trim(buffer))) {
        session.define(buffer);
      } else {
        session.evaluate(buffer);
      }
    } catch (const Error& error) {
      reportError(errors, buffer, error);
    }

    buffer.clear();
  }
}

}  // namespace qwq
