# Generates THIRD_PARTY_NOTICES from the copyright files vcpkg installs for
# every port in the dependency closure.  Run at build time as a script:
#
#   cmake -DVCPKG_SHARE_DIR=<installed>/<triplet>/share -DOUTPUT=<file>
#         -P cmake/ThirdPartyNotices.cmake
#
# Every port under share/ contributes its `copyright` file verbatim; the
# licence text is what vcpkg recorded for the exact version that was built.
if(NOT DEFINED VCPKG_SHARE_DIR OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "ThirdPartyNotices.cmake needs VCPKG_SHARE_DIR and OUTPUT")
endif()

file(GLOB _copyrights LIST_DIRECTORIES false "${VCPKG_SHARE_DIR}/*/copyright")
list(SORT _copyrights)

set(_text "SIM/36 third-party notices\n")
string(APPEND _text "==========================\n\n")
string(APPEND _text "SIM/36 itself is licensed under the MIT licence (see LICENSE).\n")
string(APPEND _text "The statically linked libraries below were built through vcpkg; each\n")
string(APPEND _text "section reproduces the licence file vcpkg recorded for that port.\n\n")

foreach(_file IN LISTS _copyrights)
    get_filename_component(_dir "${_file}" DIRECTORY)
    get_filename_component(_port "${_dir}" NAME)
    if(_port MATCHES "^vcpkg-")
        continue()   # build-time helper ports; nothing of theirs is linked
    endif()
    file(READ "${_file}" _body)
    string(APPEND _text "--------------------------------------------------------------------\n")
    string(APPEND _text "${_port}\n")
    string(APPEND _text "--------------------------------------------------------------------\n\n")
    string(APPEND _text "${_body}\n\n")
endforeach()

file(WRITE "${OUTPUT}" "${_text}")
message(STATUS "wrote ${OUTPUT}")
