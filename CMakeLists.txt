cmake_minimum_required(VERSION 3.21)

project(sim36 VERSION 0.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    # Static CRT on every configuration: the Windows binary must depend on
    # nothing but the operating system.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(replxx CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(cxxopts CONFIG REQUIRED)
find_package(doctest CONFIG REQUIRED)
find_package(minizip-ng CONFIG REQUIRED)
find_package(ZLIB REQUIRED)          # brought in by minizip-ng's zlib feature
find_package(Threads REQUIRED)

# One interface target carries the warning and platform policy so every
# target, including the tests, is built under the same rules.
add_library(sim36_policy INTERFACE)
if(MSVC)
    target_compile_options(sim36_policy INTERFACE /permissive- /Zc:__cplusplus /utf-8 /W4 /WX)
    target_compile_definitions(sim36_policy INTERFACE
        _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(sim36_policy INTERFACE -Wall -Wextra -Wpedantic -Werror)
endif()

# ---------------------------------------------------------------------------
# The emulator proper: everything except main().  The tests link it too.
# ---------------------------------------------------------------------------
add_library(sim36core STATIC
    src/Configuration/EmulatorConfig.cpp
    src/Configuration/IplSourceTable.cpp
    src/Configuration/ModelTable.cpp
    src/Devices/DeviceCodes.cpp
    src/Host/Console.cpp
    src/Host/Sockets.cpp
    src/Host/StationBackend.cpp
    src/Host/StationMultiplexer.cpp
    src/Monitor/CommandLine.cpp
    src/Monitor/CommandRegistry.cpp
    src/Monitor/ConfigurationRenderer.cpp
    src/Monitor/SimulatorSession.cpp
    src/Monitor/Tracer.cpp
    src/Storage/DiskBackend.cpp
    src/Storage/Ebcdic.cpp
    src/Storage/Vtoc.cpp
)
target_include_directories(sim36core PUBLIC src)
target_link_libraries(sim36core
    PUBLIC  sim36_policy fmt::fmt nlohmann_json::nlohmann_json
            MINIZIP::minizip-ng ZLIB::ZLIB Threads::Threads
    PRIVATE replxx::replxx)
if(WIN32)
    target_link_libraries(sim36core PUBLIC ws2_32)
endif()

add_executable(sim36 src/main.cpp)
target_link_libraries(sim36 PRIVATE sim36core cxxopts::cxxopts)

# ---------------------------------------------------------------------------
# Unit tests (doctest).
# ---------------------------------------------------------------------------
add_executable(sim36_tests
    tests/main.cpp
    tests/CommandLineTests.cpp
    tests/ConfigTests.cpp
    tests/EbcdicTests.cpp
    tests/VtocTests.cpp
)
target_link_libraries(sim36_tests PRIVATE sim36core doctest::doctest)

enable_testing()
add_test(NAME unit COMMAND sim36_tests)
add_test(NAME empty_command_file
         COMMAND sim36 -s ${CMAKE_SOURCE_DIR}/test/empty.sim)
add_test(NAME parse_check
         COMMAND sh ${CMAKE_SOURCE_DIR}/test/parse-check.sh $<TARGET_FILE:sim36>)
# Differential tests against the reference emulator run only where it is
# available (SIM36_REFERENCE); elsewhere they report "skipped" and pass.
add_test(NAME config_parity COMMAND sh ${CMAKE_SOURCE_DIR}/test/config-parity.sh)
add_test(NAME config_volume COMMAND sh ${CMAKE_SOURCE_DIR}/test/config-volume.sh)
set_tests_properties(config_parity config_volume PROPERTIES
    ENVIRONMENT "SIM36=$<TARGET_FILE:sim36>")

# ---------------------------------------------------------------------------
# THIRD_PARTY_NOTICES, generated from the vcpkg copyright files of the
# dependency closure at build time.
# ---------------------------------------------------------------------------
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(SIM36_VCPKG_SHARE "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
    set(SIM36_NOTICES "${CMAKE_BINARY_DIR}/THIRD_PARTY_NOTICES")
    add_custom_command(
        OUTPUT "${SIM36_NOTICES}"
        COMMAND ${CMAKE_COMMAND}
                -DVCPKG_SHARE_DIR=${SIM36_VCPKG_SHARE}
                -DOUTPUT=${SIM36_NOTICES}
                -P ${CMAKE_SOURCE_DIR}/cmake/ThirdPartyNotices.cmake
        DEPENDS ${CMAKE_SOURCE_DIR}/cmake/ThirdPartyNotices.cmake
        COMMENT "Generating THIRD_PARTY_NOTICES from vcpkg copyright files"
        VERBATIM)
    add_custom_target(third_party_notices ALL DEPENDS "${SIM36_NOTICES}")
    install(FILES "${SIM36_NOTICES}" DESTINATION .)
else()
    message(WARNING "not building through vcpkg: THIRD_PARTY_NOTICES will not be generated")
endif()

install(TARGETS sim36 RUNTIME DESTINATION .)
install(FILES LICENSE README.md DESTINATION .)
