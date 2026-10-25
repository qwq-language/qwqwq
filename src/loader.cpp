#include "loader.hpp"

#include <fstream>
#include <sstream>
#include <utility>

#include <sys/stat.h>

#include "error.hpp"
#include "lexer.hpp"
#include "parser.hpp"

namespace qwq {
namespace {

/// Directory part of a path, without the trailing separator; empty when the
/// path has no directory. Written by hand so the loader stays usable in builds
/// without `<filesystem>`.
std::string directoryOf(const std::string& path) {
  const std::size_t cut = path.find_last_of("/\\");
  return cut == std::string::npos ? std::string() : path.substr(0, cut);
}

std::string joinPath(const std::string& directory, const std::string& path) {
  if (path.empty() || path.front() == '/' ||
      (path.size() > 1 && path[1] == ':')) {
    return path;
  }
  if (directory.empty()) return path;
  const char last = directory.back();
  if (last == '/' || last == '\\') return directory + path;
  return directory + "/" + path;
}

bool isHeaderName(const std::string& path) {
  return path.size() > 4 && path.compare(path.size() - 4, 4, ".awa") == 0;
}

/// Loads a program by walking its includes. `loading` holds the files being
/// read, so an include cycle is reported instead of recursing forever.
class Loader {
 public:
  Loader(const SourceProvider& provider, const std::vector<std::string>& searchPaths)
      : provider_(provider), searchPaths_(searchPaths) {}

  void loadText(const std::string& source, const std::string& path,
                Program& program, bool entry) {
    Program parsed = parse(tokenize(source, /*keepComments=*/false),
                           isHeaderName(path));
    if (!entry) {
      for (TypeAliasDefinition& alias : parsed.typeAliases) {
        program.typeAliases.push_back(std::move(alias));
      }
    }

    for (const Include& include : parsed.includes) {
      loadInclude(include, path, program);
    }

    for (Definition& definition : parsed.definitions) {
      definition.fromEntry = entry;
      program.definitions.push_back(std::move(definition));
    }
    if (entry) {
      program.typeAliases.insert(program.typeAliases.end(),
                                 std::make_move_iterator(parsed.typeAliases.begin()),
                                 std::make_move_iterator(parsed.typeAliases.end()));
      program.main = std::move(parsed.main);
    }
  }

 private:
  void loadInclude(const Include& include, const std::string& from,
                   Program& program) {
    const std::optional<std::string> resolved = resolve(include, from);
    if (!resolved) {
      throw Error("cannot open header '" + include.path + "' (looked in " +
                      candidates(include, from) + ")",
                  include.position);
    }
    if (resolved->empty()) return;  // already loaded

    loading_.push_back(*resolved);
    const std::optional<std::string> text = provider_.read(*resolved);
    if (!text) {
      loading_.pop_back();
      throw Error("cannot open header '" + include.path + "' (looked in " +
                      candidates(include, from) + ")",
                  include.position);
    }
    loadText(*text, *resolved, program, /*entry=*/false);
    loading_.pop_back();
  }

  std::string candidates(const Include& include,
                         const std::string& from) const {
    std::string list = joinPath(directoryOf(from), include.path);
    for (const std::string& directory : searchPaths_) {
      list += ", " + joinPath(directory, include.path);
    }
    return list;
  }

  /// The first path that exists; an empty string when it was loaded before;
  /// nothing when no candidate exists.
  std::optional<std::string> resolve(const Include& include,
                                     const std::string& from) {
    for (const std::string& candidate : searchOrder(include, from)) {
      if (provider_.read(candidate)) {
        for (const std::string& active : loading_) {
          if (active == candidate) {
            throw Error("header '" + candidate + "' includes itself",
                        include.position);
          }
        }
        for (const std::string& loaded : loaded_) {
          if (loaded == candidate) return {};
        }
        loaded_.push_back(candidate);
        return candidate;
      }
    }
    return std::nullopt;
  }

  std::vector<std::string> searchOrder(const Include& include,
                                       const std::string& from) const {
    std::vector<std::string> order;
    order.push_back(joinPath(directoryOf(from), include.path));
    for (const std::string& directory : searchPaths_) {
      order.push_back(joinPath(directory, include.path));
    }
    return order;
  }

  const SourceProvider& provider_;
  const std::vector<std::string>& searchPaths_;
  std::vector<std::string> loaded_;
  std::vector<std::string> loading_;
};

/// Expands `=> header` lines into the header text itself.
std::string expand(const std::string& source, const std::string& from,
                   const SourceProvider& provider,
                   const std::vector<std::string>& searchPaths,
                   std::vector<std::string>& active) {
  Program parsed = parse(tokenize(source, /*keepComments=*/false),
                         isHeaderName(from));
  std::string text = source;
  // Splice from the last include backwards so earlier offsets stay valid.
  for (auto include = parsed.includes.rbegin(); include != parsed.includes.rend();
       ++include) {
    std::string found;
    std::vector<std::string> order;
    order.push_back(joinPath(directoryOf(from), include->path));
    for (const std::string& directory : searchPaths) {
      order.push_back(joinPath(directory, include->path));
    }
    for (const std::string& candidate : order) {
      if (provider.read(candidate)) {
        found = candidate;
        break;
      }
    }
    if (found.empty()) {
      throw Error("cannot open header '" + include->path + "'", include->position);
    }
    for (const std::string& name : active) {
      if (name == found) {
        throw Error("header '" + found + "' includes itself", include->position);
      }
    }
    const std::optional<std::string> nested = provider.read(found);
    active.push_back(found);
    const std::string expansion = expand(*nested, found, provider, searchPaths, active);
    active.pop_back();

    std::size_t begin = include->position;
    while (begin > 0 && text[begin - 1] != '\n') --begin;
    std::size_t end = text.find('\n', include->position);
    if (end == std::string::npos) end = text.size();
    else ++end;
    text.replace(begin, end - begin, expansion + "\n");
  }
  return text;
}

}  // namespace

std::string inlineHeaders(const std::string& path,
                          const SourceProvider& provider,
                          const std::vector<std::string>& searchPaths) {
  std::string found;
  std::vector<std::string> order{path};
  for (const std::string& directory : searchPaths) {
    order.push_back(joinPath(directory, path));
  }
  for (const std::string& candidate : order) {
    if (provider.read(candidate)) {
      found = candidate;
      break;
    }
  }
  if (found.empty()) throw Error("cannot open header '" + path + "'");

  const std::optional<std::string> text = provider.read(found);
  std::vector<std::string> active{found};
  return expand(*text, found, provider, searchPaths, active);
}

Program loadProgram(const std::string& path, const SourceProvider& provider,
                    const std::vector<std::string>& searchPaths) {
  std::vector<std::string> order{path};
  for (const std::string& directory : searchPaths) {
    order.push_back(joinPath(directory, path));
  }
  for (const std::string& candidate : order) {
    const std::optional<std::string> text = provider.read(candidate);
    if (!text) continue;
    Loader loader(provider, searchPaths);
    Program program;
    loader.loadText(*text, candidate, program, /*entry=*/true);
    return program;
  }
  throw Error("cannot open '" + path + "'");
}

Program loadSource(const std::string& source, const std::string& path,
                   const SourceProvider& provider,
                   const std::vector<std::string>& searchPaths) {
  Loader loader(provider, searchPaths);
  Program program;
  loader.loadText(source, path, program, /*entry=*/true);
  return program;
}

std::optional<std::string> FileSystemProvider::read(
    const std::string& path) const {
  // A directory opens as a stream of zero bytes on POSIX, which would make a
  // search-path entry look like an empty header.
  struct stat info = {};
  if (stat(path.c_str(), &info) != 0) return std::nullopt;
  if ((info.st_mode & S_IFMT) != S_IFREG) return std::nullopt;

  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

void MemoryProvider::set(std::string path, std::string text) {
  for (auto& entry : files_) {
    if (entry.first == path) {
      entry.second = std::move(text);
      return;
    }
  }
  files_.emplace_back(std::move(path), std::move(text));
}

std::optional<std::string> MemoryProvider::read(const std::string& path) const {
  for (const auto& entry : files_) {
    if (entry.first == path) return entry.second;
  }
  return std::nullopt;
}

}  // namespace qwq
