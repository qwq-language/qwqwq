# qwqwq

**qwqwq** is a C++20 interpreter for the [qwq programming language](https://github.com/qwq-language).

## Build

Requirements: CMake 3.16 or newer and a C++20 compiler.

### Linux / macOS

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

### Windows — Visual Studio

```powershell
# 64-bit
cmake -S . -B build -A x64
cmake --build build --config Release --parallel

# 32-bit
cmake -S . -B build -A Win32
cmake --build build --config Release --parallel
```

### Windows — MinGW-w64

Make sure `g++` and `mingw32-make` are available on `PATH`.
The target architecture is determined by the installed MinGW compiler.

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

## Run

```sh
# Linux / macOS
./build/qwqwq path/to/program.qwq
```

```powershell
# Windows — Visual Studio
.\build\Release\qwqwq path\to\program.qwq

# Windows — MinGW-w64
.\build\qwqwq path\to\program.qwq
```

The command-line form is:

```text
qwqwq [options] [file]
```

If no file is given, qwqwq reads the program from standard input.
When standard input is an interactive terminal, it starts the REPL instead.
Use `--repl` to start the REPL explicitly.

## Options

| Option | Purpose |
| --- | --- |
| `-` | Read the program from standard input. |
| `--check` | Check the program without evaluating it. |
| `--dump` | Print the parsed syntax tree with inferred types. |
| `--fmt` | Write the source in canonical layout. |
| `--fmt-check` | Check whether the source is in canonical layout. |
| `--repl` | Start the interactive REPL. |
| `--max-steps N` | Set the maximum number of evaluation steps. |
| `-h`, `--help` | Show command-line help. |
| `-v`, `--version` | Show the interpreter version. |

Run `qwqwq --help` for the options supported by the downloaded build.
Inside the REPL, enter `:help` for REPL commands.

## Downloads

Prebuilt binaries are published on the [GitHub Releases page](https://github.com/qwq-language/qwqwq/releases).
Choose the archive matching your operating system and architecture.
The executable is named `qwqwq.exe` on Windows and `qwqwq` on Linux and macOS.
