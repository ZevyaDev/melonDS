find_package(Lua REQUIRED)
find_package(PkgConfig REQUIRED)

if (NOT TARGET net-utils)
    add_subdirectory("../../net" ${CMAKE_BINARY_DIR}/net)
endif()

add_library(melonds_headless SHARED
    Api.cpp
    Commands.cpp
    Input.cpp
    InputApply.cpp
    Instance.cpp
    LuaHost.cpp
    Platform.cpp
    ../lua_debug/LuaDebug.cpp
)

set_target_properties(core PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(melonds_headless PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)

target_include_directories(melonds_headless PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../lua_debug"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/net"
    ${LUA_INCLUDE_DIR})

target_link_libraries(melonds_headless PRIVATE core net-utils ${LUA_LIBRARIES})

if (WIN32)
    target_link_libraries(melonds_headless PRIVATE ws2_32 iphlpapi)
    target_link_options(melonds_headless PRIVATE -static-libgcc -static-libstdc++)
endif()

# Unit tests for the pure-logic sources (no NDS needed)
add_executable(headless_tests
    tests/main.cpp
    tests/CommandQueueTests.cpp
    tests/InputQueueTests.cpp
    Commands.cpp
    Input.cpp
)
find_package(Threads REQUIRED)
target_include_directories(headless_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(headless_tests PRIVATE Threads::Threads)
if (WIN32)
    target_link_options(headless_tests PRIVATE -static-libgcc -static-libstdc++)
endif()
add_test(NAME headless_unit COMMAND headless_tests)
set_tests_properties(headless_unit PROPERTIES TIMEOUT 60)

# Python integration tests drive the DLL through the ctypes wrapper
find_package(Python3 COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
    set(MCP_DIR "${CMAKE_SOURCE_DIR}/tools/melonds-mcp")
    add_test(NAME python_integration
        COMMAND ${Python3_EXECUTABLE} -m unittest discover -s "${MCP_DIR}/tests" -t "${MCP_DIR}" -v)
    set(PY_TEST_ENV "MELONDS_HEADLESS_LIB=$<TARGET_FILE:melonds_headless>")
    if (WIN32)
        get_filename_component(RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
        list(APPEND PY_TEST_ENV "MELONDS_DLL_DIRS=${RUNTIME_DIR}")
    endif()
    set_tests_properties(python_integration PROPERTIES ENVIRONMENT "${PY_TEST_ENV}" TIMEOUT 300)
else()
    message(STATUS "Python3 not found: python_integration test disabled")
endif()

# Qt-free debug library (symbols, disassembler)
if (NOT TARGET melonds_debug)
    add_subdirectory("../debug_common" ${CMAKE_BINARY_DIR}/debug_common)
endif()
target_link_libraries(melonds_headless PRIVATE melonds_debug)
