#cmake version
CMAKE_MINIMUM_REQUIRED(VERSION 3.13)

#define project name
PROJECT(ryzenadj C CXX)

include(GNUInstallDirs)
include(CheckCCompilerFlag)

# The sources no longer use C23 [[attribute]] syntax, so a plain C11 baseline
# is enough and keeps older toolchains working.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

option(BUILD_SHARED_LIBS "Build using shared libraries" ON)
option(PREFER_STATIC_LINKING "Prefer static linking for the ryzenadj executable" OFF)
option(RYZENADJ_HARDENING "Enable compiler/linker hardening for this root-privileged tool" ON)

# LTO with no optimisation level is a pointless combination, and
# _FORTIFY_SOURCE silently does nothing without -O. Default to Release when
# neither the user nor the distro build system picked something.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# ---------------------------------------------------------------------------
# Hardening
#
# ryzenadj runs as root and pokes the SMU through PCI config space and
# /dev/mem, so it is exactly the kind of binary that should carry the full set
# of mitigations. Everything below is *appended* via add_compile_options() /
# add_link_options() instead of being assigned to CMAKE_C_FLAGS, so distro
# flags (Gentoo make.conf, dpkg-buildflags, ...) are preserved rather than
# overwritten - and, unlike a Makefile `?=`, an environment-supplied CFLAGS
# cannot silently delete them.
# ---------------------------------------------------------------------------
function(ryzenadj_try_add_flag flag)
    string(MAKE_C_IDENTIFIER "HAVE_FLAG${flag}" varname)
    check_c_compiler_flag("${flag}" ${varname})
    if(${varname})
        add_compile_options(${flag})
    endif()
endfunction()

# Warnings: these caught several of the defects fixed in this tree (a dead
# error check, sign-compare bugs, format/varargs mismatches). Keep them on.
foreach(warn -Wall -Wextra -Wformat=2 -Wno-missing-field-initializers)
    ryzenadj_try_add_flag(${warn})
endforeach()

if(RYZENADJ_HARDENING AND NOT MSVC)
    # This code type-puns its way around MMIO and PM-table buffers. Combined
    # with the LTO enabled below, strict aliasing is a realistic
    # miscompilation risk here for very little performance upside.
    ryzenadj_try_add_flag(-fno-strict-aliasing)

    ryzenadj_try_add_flag(-fstack-protector-strong)
    ryzenadj_try_add_flag(-fstack-clash-protection)
    ryzenadj_try_add_flag(-fcf-protection=full)

    # _FORTIFY_SOURCE needs optimisation; skip it for Debug builds where it
    # would only produce a "requires -O" warning.
    add_compile_definitions($<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=2>)

    if(NOT WIN32 AND NOT APPLE)
        add_link_options(-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
    endif()
endif()

#Enable LTO
include(CheckIPOSupported)
check_ipo_supported(RESULT supported OUTPUT error)
if( supported )
    message(STATUS "IPO / LTO enabled")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

AUX_SOURCE_DIRECTORY(./ SRC_DIR)

if(WIN32)
set(OS_SOURCE lib/win32/osdep_win32.cpp)
set(OS_LINK_LIBRARY WinRing0x64)
set(OS_LINK_DIR ./win32)
LINK_DIRECTORIES(${OS_LINK_DIR})
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
set(OS_SOURCE lib/linux/osdep_linux.c lib/linux/osdep_linux_mem.c lib/linux/osdep_linux_smu_kernel_module.c)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPCI REQUIRED libpci)

add_library(ryzenadj_libpci INTERFACE)
add_library(RyzenAdj::Libpci ALIAS ryzenadj_libpci)
target_include_directories(ryzenadj_libpci INTERFACE ${LIBPCI_INCLUDE_DIRS})
target_compile_options(ryzenadj_libpci INTERFACE ${LIBPCI_CFLAGS_OTHER})
target_link_libraries(ryzenadj_libpci INTERFACE ${LIBPCI_LDFLAGS})

add_library(ryzenadj_libpci_static INTERFACE)
add_library(RyzenAdj::LibpciStatic ALIAS ryzenadj_libpci_static)
target_include_directories(ryzenadj_libpci_static INTERFACE ${LIBPCI_STATIC_INCLUDE_DIRS})
target_compile_options(ryzenadj_libpci_static INTERFACE ${LIBPCI_STATIC_CFLAGS_OTHER})
target_link_libraries(ryzenadj_libpci_static INTERFACE ${LIBPCI_STATIC_LDFLAGS})

set(OS_LINK_LIBRARY RyzenAdj::Libpci)
set(OS_STATIC_LINK_LIBRARY RyzenAdj::LibpciStatic)
else()
message(FATAL_ERROR "Unsupported operating system: ${CMAKE_SYSTEM_NAME}")
endif()

set(COMMON_SOURCES lib/nb_smu_ops.c lib/api.c lib/cpuid.c)
add_definitions(-D_LIBRYZENADJ_INTERNAL)

ADD_EXECUTABLE(${PROJECT_NAME} ${OS_SOURCE} ${COMMON_SOURCES} argparse.c main.c)

# A fully static binary cannot be a PIE, so only ask for PIE otherwise.
if(RYZENADJ_HARDENING AND NOT PREFER_STATIC_LINKING)
    set_property(TARGET ${PROJECT_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)
    if(NOT WIN32 AND NOT APPLE)
        set_property(TARGET ${PROJECT_NAME} APPEND_STRING PROPERTY LINK_FLAGS " -pie")
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND (PREFER_STATIC_LINKING OR NOT BUILD_SHARED_LIBS))
    if(PREFER_STATIC_LINKING)
        set_property(TARGET ${PROJECT_NAME} APPEND_STRING PROPERTY LINK_FLAGS " -static")
    endif()
    target_link_libraries(${PROJECT_NAME} ${OS_STATIC_LINK_LIBRARY})
else()
    target_link_libraries(${PROJECT_NAME} ${OS_LINK_LIBRARY})
endif()
#SET_TARGET_PROPERTIES(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE C)
ADD_LIBRARY (libryzenadj ${OS_SOURCE} ${COMMON_SOURCES})
set_target_properties(libryzenadj PROPERTIES PREFIX "")
set_property(TARGET libryzenadj PROPERTY POSITION_INDEPENDENT_CODE ON)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT BUILD_SHARED_LIBS)
    target_link_libraries(libryzenadj ${OS_STATIC_LINK_LIBRARY})
else()
    target_link_libraries(libryzenadj ${OS_LINK_LIBRARY})
endif()
#SET_TARGET_PROPERTIES(libryzenadj PROPERTIES LINKER_LANGUAGE C)
install(TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
