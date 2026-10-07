# Unit tests (doctest, single-header).
add_executable(bootroll_tests
    test_hive.cpp
    test_bcd_store.cpp
    test_partition_table.cpp
    test_bootcode.cpp
    test_hexedit.cpp
    test_uefi_vars.cpp
    test_loader_presets.cpp
    test_fat.cpp
    ${CMAKE_SOURCE_DIR}/src/core/bcd/Hive.cpp
    ${CMAKE_SOURCE_DIR}/src/core/bcd/Utf16.cpp
    ${CMAKE_SOURCE_DIR}/src/core/bcd/BcdStore.cpp
    ${CMAKE_SOURCE_DIR}/src/core/bcd/BcdElements.cpp
    ${CMAKE_SOURCE_DIR}/src/core/util/HexText.cpp
    ${CMAKE_SOURCE_DIR}/src/core/hexedit/HexEdit.cpp
    ${CMAKE_SOURCE_DIR}/src/core/uefi/UefiVars.cpp
    ${CMAKE_SOURCE_DIR}/src/core/uefi/LoaderPresets.cpp
    ${CMAKE_SOURCE_DIR}/src/core/fat/FatVolume.cpp
    ${CMAKE_SOURCE_DIR}/src/core/disk/PartitionTable.cpp
    ${CMAKE_SOURCE_DIR}/src/core/bootcode/BootCode.cpp
    ${CMAKE_SOURCE_DIR}/src/core/bootcode/PbrCode.cpp
    ${CMAKE_BINARY_DIR}/generated/bootcode_nt6_mbr.cpp
    ${CMAKE_BINARY_DIR}/generated/bootcode_grub4dos_mbr.cpp
    ${CMAKE_BINARY_DIR}/generated/bootcode_grub4dos_pbr.cpp
    ${CMAKE_BINARY_DIR}/generated/bootcode_grub4dos_grldr.cpp
    ${CMAKE_BINARY_DIR}/generated/bootcode_wee63_mbr.cpp
    ${CMAKE_BINARY_DIR}/generated/bootcode_syslinux_mbr.cpp)
set_source_files_properties(
    ${CMAKE_BINARY_DIR}/generated/bootcode_nt6_mbr.cpp
    ${CMAKE_BINARY_DIR}/generated/bootcode_grub4dos_mbr.cpp
    ${CMAKE_BINARY_DIR}/generated/bootcode_grub4dos_pbr.cpp
    ${CMAKE_BINARY_DIR}/generated/bootcode_grub4dos_grldr.cpp
    ${CMAKE_BINARY_DIR}/generated/bootcode_wee63_mbr.cpp
    ${CMAKE_BINARY_DIR}/generated/bootcode_syslinux_mbr.cpp
    PROPERTIES GENERATED TRUE)
target_include_directories(bootroll_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/doctest
    ${CMAKE_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(bootroll_tests PRIVATE /W0)
endif()
set_target_properties(bootroll_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The real-BCD fixture compiles in only when a BCD copy is provided at build time.
if(DEFINED ENV{BOOTROLL_TEST_REAL_BCD})
    target_compile_definitions(bootroll_tests PRIVATE BOOTROLL_TEST_REAL_BCD=1)
    message(STATUS "tests: real-BCD fixture enabled ($ENV{BOOTROLL_TEST_REAL_BCD})")
endif()

include(CTest)
add_test(NAME hive COMMAND bootroll_tests)
