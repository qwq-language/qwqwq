cmake_minimum_required(VERSION 3.16)
project(qwqwq LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(qwqwq
  src/ast.cpp
  src/checker.cpp
  src/diagnostic.cpp
  src/fmt.cpp
  src/interpreter.cpp
  src/lexer.cpp
  src/main.cpp
  src/parser.cpp
  src/repl.cpp
  src/type.cpp
  src/value.cpp
)

if(MSVC)
    target_link_options(qwqwq PRIVATE "/STACK:1073741824")
elseif(MINGW)
    target_link_options(qwqwq PRIVATE "-Wl,--stack,1073741824")
elseif(APPLE)
    target_link_options(qwqwq PRIVATE "LINKER:-stack_size,0x4000000")
endif()

target_include_directories(qwqwq PRIVATE src/include)

target_compile_options(qwqwq PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
)