find_package(PkgConfig REQUIRED)

add_library(melonds_debug STATIC
    DebugSymbols.cpp
    DebugSymbolsRom.cpp
    Disassembler.cpp
)

set_target_properties(melonds_debug PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(melonds_debug PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src")

target_link_libraries(melonds_debug PUBLIC core)

pkg_check_modules(Capstone IMPORTED_TARGET GLOBAL capstone)
if (Capstone_FOUND)
    target_link_libraries(melonds_debug PUBLIC PkgConfig::Capstone)
    target_compile_definitions(melonds_debug PRIVATE MELONDS_HAVE_CAPSTONE)
else()
    message(WARNING "capstone not found: the debugger shows raw opcodes instead of disassembly")
endif()

if (BUILD_HEADLESS)
    add_executable(debug_common_tests tests/DebugCommonTests.cpp tests/OverlayMatchTests.cpp)
    target_link_libraries(debug_common_tests PRIVATE melonds_debug)
    if (WIN32)
        target_link_options(debug_common_tests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME debug_common_tests COMMAND debug_common_tests)
endif()
