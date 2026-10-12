# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: GPL-3.0+

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../externals/uam/source/compiler_iface.cpp")
	set(_uam_default_root "${CMAKE_CURRENT_LIST_DIR}/../externals/uam")
else()
	set(_uam_default_root "${CMAKE_SOURCE_DIR}/../uam")
endif()
set(UAM_ROOT "${_uam_default_root}" CACHE PATH
	"Checkout of https://github.com/PalindromicBreadLoaf/uam (c0d76aa or later)")
option(ARMSX2_USE_PACKAGED_UAM "Compile DK3D shaders with devkitPro's uam." OFF)

if(ARMSX2_USE_PACKAGED_UAM)
	find_program(UAM_EXE NAMES uam PATHS "$ENV{DEVKITPRO}/tools/bin" "${DEVKITPRO}/tools/bin" "/opt/devkitpro/tools/bin")
	if(NOT UAM_EXE)
		message(FATAL_ERROR "uam not found.")
	endif()
	message(WARNING "Using the packaged uam (${UAM_EXE}). Deko3D may exhibit visual bugs.")
	set(UAM_DEPENDS "")
	return()
endif()

if(NOT EXISTS "${UAM_ROOT}/source/compiler_iface.cpp")
	message(FATAL_ERROR
		"uam sources not found at ${UAM_ROOT}.\n"
		"Clone https://github.com/PalindromicBreadLoaf/uam there or set UAM_ROOT. "
		"-DARMSX2_USE_PACKAGED_UAM=ON uses devkitPro's uam instead, which breaks some effects.")
endif()

find_program(UAM_MESON NAMES meson)
find_program(UAM_NINJA NAMES ninja)
find_program(UAM_BISON NAMES bison)
find_program(UAM_FLEX NAMES flex)
foreach(_tool UAM_MESON UAM_NINJA UAM_BISON UAM_FLEX)
	if(NOT ${_tool})
		message(FATAL_ERROR "${_tool} not found. meson, ninja, bison and flex are needed to build the host uam.")
	endif()
endforeach()

include(ExternalProject)
set(_uam_host_dir "${CMAKE_BINARY_DIR}/uam-host")
set(UAM_EXE "${_uam_host_dir}/uam")
ExternalProject_Add(uam_host
	SOURCE_DIR "${UAM_ROOT}"
	BINARY_DIR "${_uam_host_dir}"
	CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env --unset=CC --unset=CXX
		"${UAM_MESON}" setup --buildtype=release "${_uam_host_dir}" "${UAM_ROOT}"
	BUILD_COMMAND "${CMAKE_COMMAND}" -E env --unset=CC --unset=CXX "${UAM_NINJA}" -C "${_uam_host_dir}"
	INSTALL_COMMAND ""
	BUILD_ALWAYS ON
	BUILD_BYPRODUCTS "${UAM_EXE}"
)
set(UAM_DEPENDS uam_host "${UAM_EXE}")
