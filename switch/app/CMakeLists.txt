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
dk3d_add_shader(convert_fsh frag)
dk3d_add_shader(convert_int_fsh frag)
dk3d_add_shader(downsample_fsh frag)
dk3d_add_shader(clut_fsh frag)
dk3d_add_shader(convert_8i_fsh frag)
dk3d_add_shader(tfx_vsh vert)
dk3d_add_shader(tfx_fsh frag)
dk3d_add_shader(interlace_fsh frag)
dk3d_add_shader(shadeboost_fsh frag)
dk3d_add_shader(fxaa_fsh frag)
dk3d_add_shader(merge_fsh frag)
dk3d_add_shader(imgui_vsh vert)
dk3d_add_shader(imgui_fsh frag)

# CAS needs the AMD FidelityFX headers inlined
set(DK3D_FFX_DIR "${CMAKE_SOURCE_DIR}/../assets/resources/shaders/common")
set(DK3D_CAS_GEN "${CMAKE_CURRENT_BINARY_DIR}/cas_csh.gen.glsl")
add_custom_command(
	OUTPUT "${DK3D_CAS_GEN}"
	COMMAND "${CMAKE_COMMAND}"
		-DIN=${DK3D_SHADER_SRC_DIR}/cas_csh.glsl
		-DFFX_A=${DK3D_FFX_DIR}/ffx_a.h
		-DFFX_CAS=${DK3D_FFX_DIR}/ffx_cas.h
		-DOUT=${DK3D_CAS_GEN}
		-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/inline_shader_includes.cmake"
	DEPENDS "${DK3D_SHADER_SRC_DIR}/cas_csh.glsl" "${DK3D_FFX_DIR}/ffx_a.h" "${DK3D_FFX_DIR}/ffx_cas.h"
		"${CMAKE_CURRENT_SOURCE_DIR}/cmake/inline_shader_includes.cmake"
	COMMENT "Inlining ffx includes"
	VERBATIM
)
add_custom_command(
	OUTPUT "${DK3D_ROMFS_SHADER_DIR}/cas_csh.dksh"
	COMMAND "${UAM_EXE}" -s comp -o "${DK3D_ROMFS_SHADER_DIR}/cas_csh.dksh" "${DK3D_CAS_GEN}"
	DEPENDS "${DK3D_CAS_GEN}"
	COMMENT "uam comp cas_csh.dksh"
	VERBATIM
)
list(APPEND DK3D_DKSH_FILES "${DK3D_ROMFS_SHADER_DIR}/cas_csh.dksh")

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
