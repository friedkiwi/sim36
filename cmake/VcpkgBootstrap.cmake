# Select vcpkg early enough for project() to load its toolchain.  Callers can
# still use another toolchain by setting CMAKE_TOOLCHAIN_FILE explicitly.
if(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT "${CMAKE_TOOLCHAIN_FILE}" STREQUAL "")
    # CMake only loads a newly selected toolchain while creating a build tree.
    # If this cache predates the bootstrap, activate vcpkg for this configure
    # pass too so the user does not have to delete the cache or run CMake twice.
    if(DEFINED CMAKE_PROJECT_NAME AND
       "${CMAKE_TOOLCHAIN_FILE}" MATCHES "vcpkg\\.cmake$" AND
       NOT DEFINED VCPKG_INSTALLED_DIR)
        if(APPLE AND NOT VCPKG_TARGET_TRIPLET)
            execute_process(COMMAND uname -m
                            OUTPUT_VARIABLE _sim36_host_arch
                            OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(_sim36_host_arch STREQUAL "arm64")
                set(VCPKG_TARGET_TRIPLET "arm64-osx")
            elseif(_sim36_host_arch STREQUAL "x86_64")
                set(VCPKG_TARGET_TRIPLET "x64-osx")
            endif()
        endif()
        include("${CMAKE_TOOLCHAIN_FILE}")
    endif()
    return()
endif()

set(_sim36_vcpkg_root "")
if(DEFINED VCPKG_ROOT AND
   EXISTS "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(_sim36_vcpkg_root "${VCPKG_ROOT}")
elseif(DEFINED ENV{VCPKG_ROOT} AND
       EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(_sim36_vcpkg_root "$ENV{VCPKG_ROOT}")
endif()

if(NOT _sim36_vcpkg_root)
    find_package(Git QUIET)
    if(NOT Git_FOUND)
        message(FATAL_ERROR
            "vcpkg is not available and Git is required to bootstrap it. "
            "Install Git or set VCPKG_ROOT/CMAKE_TOOLCHAIN_FILE.")
    endif()

    file(READ "${CMAKE_CURRENT_LIST_DIR}/../vcpkg.json" _sim36_manifest)
    string(JSON _sim36_vcpkg_baseline ERROR_VARIABLE _sim36_json_error
           GET "${_sim36_manifest}" builtin-baseline)
    if(_sim36_json_error)
        message(FATAL_ERROR
            "Could not read builtin-baseline from vcpkg.json: ${_sim36_json_error}")
    endif()

    set(_sim36_vcpkg_root "${CMAKE_BINARY_DIR}/_vcpkg")
    if(NOT EXISTS "${_sim36_vcpkg_root}/.git")
        message(STATUS "Fetching vcpkg ${_sim36_vcpkg_baseline}")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" clone https://github.com/microsoft/vcpkg.git
                    "${_sim36_vcpkg_root}"
            RESULT_VARIABLE _sim36_clone_result
            COMMAND_ERROR_IS_FATAL ANY)
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" checkout --detach "${_sim36_vcpkg_baseline}"
        WORKING_DIRECTORY "${_sim36_vcpkg_root}"
        OUTPUT_QUIET
        RESULT_VARIABLE _sim36_checkout_result
        COMMAND_ERROR_IS_FATAL ANY)

    if(WIN32)
        execute_process(
            COMMAND cmd /c bootstrap-vcpkg.bat -disableMetrics
            WORKING_DIRECTORY "${_sim36_vcpkg_root}"
            RESULT_VARIABLE _sim36_bootstrap_result
            COMMAND_ERROR_IS_FATAL ANY)
    else()
        execute_process(
            COMMAND sh bootstrap-vcpkg.sh -disableMetrics
            WORKING_DIRECTORY "${_sim36_vcpkg_root}"
            RESULT_VARIABLE _sim36_bootstrap_result
            COMMAND_ERROR_IS_FATAL ANY)
    endif()
endif()

set(VCPKG_ROOT "${_sim36_vcpkg_root}" CACHE PATH "vcpkg installation used by SIM36")
set(CMAKE_TOOLCHAIN_FILE
    "${_sim36_vcpkg_root}/scripts/buildsystems/vcpkg.cmake"
    CACHE FILEPATH "CMake toolchain file")

if(DEFINED CMAKE_PROJECT_NAME AND NOT DEFINED VCPKG_INSTALLED_DIR)
    if(APPLE AND NOT VCPKG_TARGET_TRIPLET)
        execute_process(COMMAND uname -m
                        OUTPUT_VARIABLE _sim36_host_arch
                        OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(_sim36_host_arch STREQUAL "arm64")
            set(VCPKG_TARGET_TRIPLET "arm64-osx")
        elseif(_sim36_host_arch STREQUAL "x86_64")
            set(VCPKG_TARGET_TRIPLET "x64-osx")
        endif()
    endif()
    include("${CMAKE_TOOLCHAIN_FILE}")
endif()

unset(_sim36_manifest)
unset(_sim36_json_error)
unset(_sim36_host_arch)
unset(_sim36_vcpkg_baseline)
unset(_sim36_vcpkg_root)
