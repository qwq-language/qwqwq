#include "repl.hpp"

#include <iostream>
#include <limits>
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
    "  :quit            leave the REPL\n"
    "  :type <expr>     print the type of an expression without running it\n"
    "  :dump <expr>     print the syntax tree with inferred types\n"
    "  :defs            list the names defined in this session\n"
    "\n"
    "Anything else is parsed as qwq source.\n"
    "Functions and class types stay in scope for later lines.\n"
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

/// A line starts with a declaration when parsing finds a class declaration or a
/// function definition. Parsing is the only honest test now that neither uses
/// a declaration keyword.
bool startsWithDefinition(const std::string& text) {
  try {
    const std::vector<Token> tokens = tokenize(text);
    const Program program = parse(tokens);
    return !program.typeAliases.empty() || !program.definitions.empty();
  } catch (const Error&) {
    return false;
  }
}

/// A REPL session: the definitions written so far, plus the machinery to
/// check and run later lines against them.
class Session {
 public:
  Session(std::ostream& output, std::ostream& errors, std::istream& input,
          std::int64_t stepLimit, bool promptForInput)
      : output_(output),
        errors_(errors),
        input_(input),
        stepLimit_(stepLimit),
        promptForInput_(promptForInput) {}

  /// Validates a declaration and keeps it in scope for later lines.
  void define(const std::string& source) {
    const std::size_t sourceOffset = definitions_.size();
    const std::string combined = definitions_ + source + "\n";
    Program program = parse(tokenize(combined));
    check(program);

    const TypeAliasDefinition* lastAlias = nullptr;
    for (const TypeAliasDefinition& alias : program.typeAliases) {
      if (alias.position >= sourceOffset) lastAlias = &alias;
    }
    const Definition* lastDefinition = nullptr;
    for (const Definition& definition : program.definitions) {
      if (definition.position >= sourceOffset) lastDefinition = &definition;
    }
    if (lastAlias == nullptr && lastDefinition == nullptr) {
      errors_ << "error: expected a function definition or class declaration\n";
      return;
    }

    definitions_ = combined;
    if (lastAlias != nullptr &&
        (lastDefinition == nullptr || lastAlias->position > lastDefinition->position)) {
      show(*lastAlias);
    } else {
      show(*lastDefinition);
    }
  }

  void listDefinitions() const {
    if (definitions_.empty()) {
      output_ << "no definitions or class types\n";
      return;
    }
    Program program = parse(tokenize(definitions_));
    check(program);
    std::size_t aliasIndex = 0;
    std::size_t definitionIndex = 0;
    while (aliasIndex < program.typeAliases.size() ||
           definitionIndex < program.definitions.size()) {
      const bool printAlias =
          definitionIndex == program.definitions.size() ||
          (aliasIndex < program.typeAliases.size() &&
           program.typeAliases[aliasIndex].position <
               program.definitions[definitionIndex].position);
      if (printAlias) {
        show(program.typeAliases[aliasIndex++]);
      } else {
        show(program.definitions[definitionIndex++]);
      }
    }
  }

  void typeOf(const std::string& line) {
    const Program program = parseChecked(line);
    const Expr& main = *program.main;
    output_ << toString(main.in) << " -> "
            << toString(main.out) << "\n";
  }

  void dumpOf(const std::string& line) {
    const Program program = parseChecked(line);
    dump(*program.main, output_);
  }

  void evaluate(const std::string& line) {
    const Program program = parseChecked(line);
    const Expr& main = *program.main;

    output_ << "[" << toString(main.in) << " -> "
            << toString(main.out) << "]\n";

    if (!main.in.empty()) {
      output_ << "(expects an input stream of type "
              << toString(main.in)
              << "; not evaluated)\n";
      return;
    }

    Interpreter interpreter(output_, input_, stepLimit_, program.definitions,
                            promptForInput_ ? &output_ : nullptr,
                            &program.specializations);
    Stream result;
    try {
      result = interpreter.run(main, {});
    } catch (...) {
      finishInputLine(interpreter);
      throw;
    }
    finishInputLine(interpreter);

    if (result.empty()) return;

    for (const Value& value : result) output_ << value;
    output_ << "\n";
  }

 private:
  std::ostream& output_;
  std::ostream& errors_;
  std::istream& input_;
  std::int64_t stepLimit_;
  bool promptForInput_;
  std::string definitions_;

  /// The remaining characters on the data line must not be mistaken for the
  /// next REPL command. Do this after the entire expression has run so several
  /// `#` reads may still consume tokens from one line.
  void finishInputLine(const Interpreter& interpreter) {
    if (interpreter.inputReadCount() == 0) return;
    input_.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
  }

  /// Parses and checks `line` in the scope of the session's definitions.
  Program parseChecked(const std::string& line) {
    Program program = parse(tokenize(definitions_ + line));
    if (!program.main) throw Error("expected an expression");
    check(program);
    return program;
  }

  void show(const TypeAliasDefinition& alias) const {
    output_ << alias.name;
    if (!alias.parameters.empty()) {
      output_ << "<";
      for (std::size_t i = 0; i < alias.parameters.size(); ++i) {
        if (i != 0) output_ << ", ";
        output_ << alias.parameters[i];
      }
      output_ << ">";
    }
    output_ << " = " << portSource(alias.body) << "\n";
  }

  void show(const Definition& definition) const {
    output_ << definition.name;
    if (!definition.templateParameters.empty()) {
      output_ << "<";
      for (std::size_t i = 0; i < definition.templateParameters.size(); ++i) {
        if (i != 0) output_ << ", ";
        output_ << definition.templateParameters[i];
      }
      output_ << ">";
    }
    output_ << " : " << toString(definition.in) << " -> "
            << toString(definition.out) << "\n";
  }
};

/// Handles a `:command` line.  Returns false when the REPL should exit.
bool handleCommand(const std::string& line, Session& session, std::ostream& output,
                   std::ostream& errors) {
  const std::size_t space = line.find_first_of(" \t");
  const std::string command = line.substr(0, space);
  const std::string argument =
      space == std::string::npos ? "" : trim(line.substr(space + 1));

  if (command == ":quit") return false;

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
            std::int64_t stepLimit, bool promptForInput) {
  output << "qwqwq (qwq language interpreter) " << kVersion << "\n"
         << "Type :help for help, :quit to exit.\n";

  Session session(output, errors, input, stepLimit, promptForInput);
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
