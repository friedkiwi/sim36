cmake_minimum_required(VERSION 3.21)

# The vcpkg toolchain has to be selected before project() enables a compiler.
# This also bootstraps vcpkg for plain `cmake ..` builds when no installation
# was supplied by the caller.
include(cmake/VcpkgBootstrap.cmake)

project(sim36 VERSION 0.2.0 LANGUAGES CXX)

# Single-config generators leave CMAKE_BUILD_TYPE empty, which compiles without
# any -O flag and makes vcpkg resolve its imported targets to the debug
# libraries.  Default to Release so a plain `cmake ..` produces an optimised
# build; multi-config generators pick their configuration at build time and are
# left alone.  This must stay ahead of the find_package() calls below, which is
# where the debug/release variant of each dependency is chosen.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build configuration (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
endif()

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
    src/Devices/DeviceSet.cpp
    src/Devices/IWorkStationBackend.cpp
    src/Devices/VirtualDiskette.cpp
    src/Devices/VirtualPrinter.cpp
    src/Devices/VirtualTape.cpp
    src/Devices/VirtualWorkstation.cpp
    src/Devices/WorkStationActions.cpp
    src/Devices/WorkStationController.cpp
    src/Devices/WorkStationIob.cpp
    src/Devices/UnitBlock.cpp
    src/Devices/VirtualFixedDisk.cpp
    src/Host/Console.cpp
    src/Host/ConsoleDisplay.cpp
    src/Host/Sockets.cpp
    src/Host/StationBackend.cpp
    src/Host/StationMultiplexer.cpp
    src/Host/Telnet5250Session.cpp
    src/Monitor/CommandLine.cpp
    src/Monitor/CommandRegistry.cpp
    src/Monitor/CspCommands.cpp
    src/Monitor/MachineCommands.cpp
    src/Monitor/MachineSnapshot.cpp
    src/Monitor/MediaCommands.cpp
    src/Monitor/PanicDump.cpp
    src/Monitor/ConfigurationRenderer.cpp
    src/Monitor/MonitorCli.cpp
    src/Monitor/WorkStationCommands.cpp
    src/Monitor/MonitorCliLive.cpp
    src/Monitor/SelfTest.cpp
    src/Monitor/SimulatorSession.cpp
    src/Monitor/SrcCode.cpp
    src/Monitor/Tracer.cpp
    src/Machine/Machine.cpp
    src/Machine/MachineState.cpp
    src/Machine/MspRegisters.cpp
    src/Machine/Scheduler.cpp
    src/Processors/InstructionSet.cpp
    src/Processors/MainStorageProcessor.cpp
    src/Processors/ControlStorage/SvcTable.cpp
    src/Processors/ControlStorage/ActionControlElement.cpp
    src/Processors/ControlStorage/ActionControlElementQueue.cpp
    src/Processors/ControlStorage/AllocationQueueElement.cpp
    src/Processors/ControlStorage/As36ControlStorageProcessor.cpp
    src/Processors/ControlStorage/As36Checkpoint.cpp
    src/Processors/ControlStorage/As36Dispatch.cpp
    src/Processors/ControlStorage/As36Storage.cpp
    src/Processors/ControlStorage/As36TaskCreate.cpp
    src/Processors/ControlStorage/As36Transfer.cpp
    src/Processors/ControlStorage/As36WorkStation.cpp
    src/Processors/ControlStorage/DirectArea.cpp
    src/Processors/ControlStorage/GuestHeap.cpp
    src/Processors/ControlStorage/GuestLowStorage.cpp
    src/Processors/ControlStorage/MapParameterList.cpp
    src/Processors/ControlStorage/NuPtt.cpp
    src/Processors/ControlStorage/ProgramBlock.cpp
    src/Processors/ControlStorage/TaskBlock.cpp
    src/Processors/ControlStorage/TransientArea.cpp
    src/Storage/DiskBackend.cpp
    src/Storage/DisketteBackend.cpp
    src/Storage/FolderTapeBackend.cpp
    src/Storage/TapeManifest.cpp
    src/Storage/Ebcdic.cpp
    src/Storage/Library.cpp
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
    tests/CompletionTests.cpp
    tests/ConfigTests.cpp
    tests/ConsoleDisplayTests.cpp
    tests/CspTests.cpp
    tests/DeviceSetTests.cpp
    tests/EbcdicTests.cpp
    tests/FixedDiskTests.cpp
    tests/MachineStateTests.cpp
    tests/MspTests.cpp
    tests/VtocTests.cpp
    tests/VirtualWorkstationTests.cpp
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
add_test(NAME disk_layer COMMAND sh ${CMAKE_SOURCE_DIR}/test/disk-layer.sh)
add_test(NAME decode_differential COMMAND sh ${CMAKE_SOURCE_DIR}/test/decode-differential.sh)
add_test(NAME msp_parity COMMAND sh ${CMAKE_SOURCE_DIR}/test/msp-parity.sh)
# Milestone 4: the storage-SVC vectors and the UDT walk need only a volume;
# the scaffolding differential needs the reference as well.
add_test(NAME storage_svcs COMMAND sh ${CMAKE_SOURCE_DIR}/test/storage-svcs.sh)
add_test(NAME udt_walk COMMAND sh ${CMAKE_SOURCE_DIR}/test/udt-walk.sh)
add_test(NAME csp_scaffolding COMMAND sh ${CMAKE_SOURCE_DIR}/test/csp-scaffolding.sh)
add_test(NAME csp_families COMMAND sh ${CMAKE_SOURCE_DIR}/test/csp-families.sh)
# Milestones 5-7: the CSP families' vector suites, the host-layer suites and
# the media suites (volume only; each skips with 77 when SIM36_VOLUME is
# unset).  The python-driven suites need a free 5250 port range.
set(SIM36_VOLUME_GATES program_block_svcs work_area_svcs task_creation task_dispatcher action_controller
    resource_svcs relocating_loader print_buffer system_queue_space system_queue_ownership
    nupterm_probes timer_transient timer_op20 timer_op38 disk_write_wrap command_files ipl_command
    workstation_actions wsc_config workstation_seam printer_seam live_monitor station_multiplex
    station_power_cycle machine_lifecycle multi_session_signon ipl_main_session ipl_unattended_main_session
    ipl_printer_rebuild
    snapshot panic_command tape tape_svc tape_operator ipl_source atr_ownership list_all_regression)
list(APPEND SIM36_VOLUME_GATES no_return_transfer history_regression catalog_regression)
foreach(gate IN LISTS SIM36_VOLUME_GATES)
    string(REPLACE "_" "-" script ${gate})
    add_test(NAME ${gate} COMMAND sh ${CMAKE_SOURCE_DIR}/test/${script}.sh)
endforeach()
set_tests_properties(storage_svcs udt_walk ${SIM36_VOLUME_GATES} PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(config_parity config_volume disk_layer decode_differential msp_parity
    storage_svcs udt_walk csp_scaffolding csp_families ${SIM36_VOLUME_GATES} PROPERTIES
    ENVIRONMENT "SIM36=$<TARGET_FILE:sim36>")
# These gates bind the same default 2300/2302..2306 station endpoints.  They
# may run alongside the non-network gates, but never alongside one another.
set(SIM36_STATION_PORT_GATES workstation_actions wsc_config workstation_seam printer_seam live_monitor
    station_multiplex station_power_cycle machine_lifecycle multi_session_signon ipl_main_session
    ipl_unattended_main_session ipl_printer_rebuild list_all_regression history_regression catalog_regression)
set_tests_properties(${SIM36_STATION_PORT_GATES} PROPERTIES RESOURCE_LOCK sim36_station_ports)

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
install(FILES LICENSE README.md RUNNING.md DESTINATION .)
install(DIRECTORY etc DESTINATION . FILES_MATCHING PATTERN "*.sim")
