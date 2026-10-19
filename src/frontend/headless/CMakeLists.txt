find_package(Lua REQUIRED)
find_package(PkgConfig REQUIRED)

if (NOT TARGET net-utils)
    add_subdirectory("../../net" ${CMAKE_BINARY_DIR}/net)
endif()

add_library(melonds_headless SHARED
    Api.cpp
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
