# ARMSX2 Switch application

add_executable(armsx2
	main.cpp
	HorizonHost.cpp
)

target_include_directories(armsx2 PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(armsx2 PRIVATE
	-Wl,--start-group
	PCSX2
	PCSX2_FLAGS
	-Wl,--end-group
	# Must precede the toolchain's -lnx
	deko3d
)

# Package an .nro
nx_generate_nacp(OUTPUT armsx2.nacp
	NAME "ARMSX2"
	AUTHOR "ARMSX2 Team \& Palindromic Bread Loaf"
	VERSION "0.1"
)
nx_create_nro(armsx2 NACP "${CMAKE_CURRENT_BINARY_DIR}/armsx2.nacp")
