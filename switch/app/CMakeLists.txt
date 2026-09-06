# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: GPL-3.0+

enable_language(ASM)

add_executable(armsx2
	main.cpp
	HorizonHost.cpp
	HorizonUsbStorage.cpp
	HorizonException.cpp
	HorizonExceptionEntry.S
)

set_source_files_properties(HorizonException.cpp HorizonExceptionEntry.S
	PROPERTIES COMPILE_OPTIONS "-fno-lto")

# USB mass-storage support
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/UsbHsFs.cmake")

target_include_directories(armsx2 PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/pcsx2"
)

set(ARMSX2_NRO_VERSION "${ARMSX2_VERSION}")
if(NOT ARMSX2_NRO_VERSION)
	set(ARMSX2_NRO_VERSION "4.0.0")
endif()

set(ARMSX2_CORE_LINK_TARGETS PCSX2)
if(TARGET PCSX2_LTO)
	list(APPEND ARMSX2_CORE_LINK_TARGETS PCSX2_LTO)
endif()

target_link_libraries(armsx2 PRIVATE
	-Wl,--start-group
	${ARMSX2_CORE_LINK_TARGETS}
	PCSX2_FLAGS
	-Wl,--end-group
	usbhsfs
)

nx_generate_nacp(OUTPUT armsx2.nacp
	NAME "ARMSX2"
	AUTHOR "ARMSX2 Team & PalindromicBreadLoaf"
	VERSION "${ARMSX2_NRO_VERSION}"
)
nx_create_nro(armsx2
	OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/armsx2.nro"
	NACP "${CMAKE_CURRENT_BINARY_DIR}/armsx2.nacp"
)
