# x64-windows-static with the toolset pinned to v142 (Visual Studio 2019).
# vcpkg would otherwise build the ports with the newest installed toolset,
# and a v143-built static library references STL helpers (__std_replace_4,
# __std_search_1, ...) that the v142 runtime does not provide, so the final
# link fails.  Everything, ports included, must be built with one toolset.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_PLATFORM_TOOLSET v142)
