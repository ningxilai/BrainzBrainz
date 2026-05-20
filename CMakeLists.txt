cmake_minimum_required(VERSION 3.12)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project(emacs-stdio-jsonrpc)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

# JSON-RPC library unit tests
add_executable(unit_tests test/unit_test.cpp)
add_executable(server_test test/server_test.cpp)

# Linux event-loop example
add_executable(example_linux example/example_linux.cpp)

# Bidirectional RPC test server
add_executable(bridge_test_server test/bridge_test_server.cpp)

if(WIN32)
  add_executable(example example/example.cpp)
endif()
