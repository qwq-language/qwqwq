#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ast.hpp"

namespace qwq {

/// Where header text comes from. The command line reads the file system; the
/// web build supplies the editor's files instead.
class SourceProvider {
 public:
  virtual ~SourceProvider() = default;
  /// The text of `path`, or nothing when it does not exist.
  virtual std::optional<std::string> read(const std::string& path) const = 0;
};

/// Reads `path` and every header it names with `=> ...`, depth first.
Program loadProgram(const std::string& path, const SourceProvider& provider,
                    const std::vector<std::string>& searchPaths);

/// Same, for source that is already in memory. `path` names it for relative
/// includes; it does not need to exist.
Program loadSource(const std::string& source, const std::string& path,
                   const SourceProvider& provider,
                   const std::vector<std::string>& searchPaths);

/// The text of `path` with every `=> header` line replaced by that header's own
/// text, depth first. The REPL keeps the result beside its own definitions.
std::string inlineHeaders(const std::string& path,
                          const SourceProvider& provider,
                          const std::vector<std::string>& searchPaths);

/// Reads from the file system.
class FileSystemProvider final : public SourceProvider {
 public:
  std::optional<std::string> read(const std::string& path) const override;
};

/// Reads from a fixed table of path -> text.
class MemoryProvider final : public SourceProvider {
 public:
  void set(std::string path, std::string text);
  std::optional<std::string> read(const std::string& path) const override;

 private:
  std::vector<std::pair<std::string, std::string>> files_;
};

}  // namespace qwq
