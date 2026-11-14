cmake_minimum_required(VERSION 3.24)

# Link spdlog/fmt statically: the plugin DLL must not depend on spdlog.dll/fmt.dll, which do not
# exist in a Skyrim install. This must be set before project().
set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")

project(Highrim VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CommonLibSSE-NG (alandtse fork) is vendored as a submodule (v15.7.3, same pin as Sleep Together).
set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLibSSE-NG tests" FORCE)
add_subdirectory(lib/commonlibsse-ng CommonLibSSE EXCLUDE_FROM_ALL)
include(lib/commonlibsse-ng/cmake/CommonLibSSE.cmake)

set(PROJECT_SOURCES
    src/PCH.h
    src/main.cpp
    src/Config.h
    src/Config.cpp
    src/Core.h
    src/Core.cpp
    src/GameData.h
    src/Prison.h
    src/Prison.cpp
    src/Blackout.h
    src/Blackout.cpp
    src/Doses.h
    src/Doses.cpp
    src/State.h
    src/State.cpp
    src/Ragdoll.h
    src/Ragdoll.cpp
    src/Withdrawal.h
    src/Withdrawal.cpp
    src/Diag.h
    src/Diag.cpp
    src/Timed.h
    src/Timed.cpp
    src/LostGear.h
    src/LostGear.cpp
)

add_commonlibsse_plugin(${PROJECT_NAME}
    AUTHOR "the3venthoriz0n"
    SOURCES ${PROJECT_SOURCES}
)

target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /permissive-)
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${PROJECT_SOURCES})

set(PACKAGE_PLUGIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/package/Data/SKSE/Plugins")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PACKAGE_PLUGIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${PROJECT_NAME}>"
            "${PACKAGE_PLUGIN_DIR}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    COMMENT "Copying Highrim.dll to package/Data/SKSE/Plugins"
)

# The plugin (MCM quest, drugs, recipes, spells, notes) and src/Forms.h are generated, not hand-written.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_esp.py"
        COMMENT "Generating package/Data/Highrim.esp and src/Forms.h"
    )
endif()

# Unit tests: pure logic only (no game headers, no PCH).
enable_testing()
add_executable(HighrimTests tests/test_main.cpp src/Config.cpp src/Core.cpp)
target_include_directories(HighrimTests PRIVATE src)
target_compile_definitions(HighrimTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_test(NAME HighrimTests COMMAND HighrimTests)
if(Python3_Interpreter_FOUND)
    add_test(NAME EspTests COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_esp.py")
    add_test(NAME GameDataTests COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_game_data.py")
    add_test(NAME SourceTests COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sources.py")
    add_test(NAME NgRelocationTests COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ng_relocations.py")
endif()
