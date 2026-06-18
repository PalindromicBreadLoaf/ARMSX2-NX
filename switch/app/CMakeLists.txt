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

# Pre-compile deko3d shaders with uam
find_program(UAM_EXE NAMES uam PATHS "$ENV{DEVKITPRO}/tools/bin" "${DEVKITPRO}/tools/bin" "/opt/devkitpro/tools/bin")
if(NOT UAM_EXE)
	message(FATAL_ERROR "uam not found. This is required to build the DK3D backend")
endif()

set(DK3D_SHADER_SRC_DIR "${CMAKE_SOURCE_DIR}/pcsx2/GS/Renderers/DK3D/shaders")
set(DK3D_ROMFS_DIR "${CMAKE_CURRENT_BINARY_DIR}/romfs")
set(DK3D_ROMFS_SHADER_DIR "${DK3D_ROMFS_DIR}/shaders")
file(MAKE_DIRECTORY "${DK3D_ROMFS_SHADER_DIR}")

set(DK3D_DKSH_FILES "")

# Compile each glsl to dksh with uam. STAGE is the -s argument.
function(dk3d_add_shader NAME STAGE)
	add_custom_command(
		OUTPUT "${DK3D_ROMFS_SHADER_DIR}/${NAME}.dksh"
		COMMAND "${UAM_EXE}" -s ${STAGE} -o "${DK3D_ROMFS_SHADER_DIR}/${NAME}.dksh" "${DK3D_SHADER_SRC_DIR}/${NAME}.glsl"
		DEPENDS "${DK3D_SHADER_SRC_DIR}/${NAME}.glsl"
		COMMENT "uam ${STAGE}: ${NAME}.glsl -> ${NAME}.dksh"
		VERBATIM
	)
	set(DK3D_DKSH_FILES ${DK3D_DKSH_FILES} "${DK3D_ROMFS_SHADER_DIR}/${NAME}.dksh" PARENT_SCOPE)
endfunction()

dk3d_add_shader(convert_vsh vert)
dk3d_add_shader(texture_fsh frag)

add_custom_target(armsx2_shaders DEPENDS ${DK3D_DKSH_FILES})
dkp_add_asset_target(armsx2_romfs "${DK3D_ROMFS_DIR}")
add_dependencies(armsx2_romfs armsx2_shaders)
set_property(TARGET armsx2_romfs APPEND PROPERTY DKP_ASSET_FILES ${DK3D_DKSH_FILES})

# Package an .nro
nx_generate_nacp(OUTPUT armsx2.nacp
	NAME "ARMSX2"
	AUTHOR "ARMSX2 Team \& PalindromicBreadLoaf"
	VERSION "0.1"
)
nx_create_nro(armsx2
	NACP "${CMAKE_CURRENT_BINARY_DIR}/armsx2.nacp"
	ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg"
	ROMFS armsx2_romfs)
