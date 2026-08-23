# ARMSX2 Switch application

add_executable(armsx2
	main.cpp
	HorizonHost.cpp
	HorizonException.cpp
	HorizonExceptionEntry.S
)

set_source_files_properties(HorizonException.cpp HorizonExceptionEntry.S
	PROPERTIES COMPILE_OPTIONS "-fno-lto")

target_include_directories(armsx2 PRIVATE ${CMAKE_SOURCE_DIR})

set(ARMSX2_CORE_LINK_TARGETS PCSX2)
if(TARGET PCSX2_LTO)
	list(APPEND ARMSX2_CORE_LINK_TARGETS PCSX2_LTO)
endif()

target_link_libraries(armsx2 PRIVATE
	-Wl,--start-group
	${ARMSX2_CORE_LINK_TARGETS}
	PCSX2_FLAGS
	-Wl,--end-group
	# Must precede the toolchain's -lnx
	deko3d
)

# Vulkan (via NXVK) backend
if(USE_VULKAN)
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(NXVK REQUIRED nxvk)

	# newlib shims required by NVK.
	target_sources(armsx2 PRIVATE nvk_compat.cpp)
	target_include_directories(armsx2 PRIVATE ${NXVK_INCLUDE_DIRS})
	target_compile_options(armsx2 PRIVATE ${NXVK_CFLAGS_OTHER})

	target_link_libraries(armsx2 PRIVATE
		${NXVK_LDFLAGS})
endif()

# uam compiled into the NRO
if(DK3D_RUNTIME_SHADERS)
	include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Uam.cmake")
	target_link_libraries(armsx2 PRIVATE uam)
endif()

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

# Generate specialised shaders for cheaper draws over the ubershader
function(dk3d_add_tfx_variant OUT_NAME DEFINE)
	set(GEN "${CMAKE_CURRENT_BINARY_DIR}/${OUT_NAME}.gen.glsl")
	add_custom_command(
		OUTPUT "${GEN}"
		COMMAND "${CMAKE_COMMAND}"
			-DIN=${DK3D_SHADER_SRC_DIR}/tfx_fsh.glsl
			-DDEF=${DEFINE}
			-DOUT=${GEN}
			-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/prepend_define.cmake"
		DEPENDS "${DK3D_SHADER_SRC_DIR}/tfx_fsh.glsl"
			"${CMAKE_CURRENT_SOURCE_DIR}/cmake/prepend_define.cmake"
		COMMENT "gen tfx variant ${OUT_NAME} (${DEFINE})"
		VERBATIM
	)
	add_custom_command(
		OUTPUT "${DK3D_ROMFS_SHADER_DIR}/${OUT_NAME}.dksh"
		COMMAND "${UAM_EXE}" -s frag -o "${DK3D_ROMFS_SHADER_DIR}/${OUT_NAME}.dksh" "${GEN}"
		DEPENDS "${GEN}"
		COMMENT "uam frag: ${OUT_NAME}.dksh"
		VERBATIM
	)
	set(DK3D_DKSH_FILES ${DK3D_DKSH_FILES} "${DK3D_ROMFS_SHADER_DIR}/${OUT_NAME}.dksh" PARENT_SCOPE)
endfunction()

dk3d_add_shader(convert_vsh vert)
dk3d_add_shader(texture_fsh frag)
dk3d_add_shader(convert_fsh frag)
dk3d_add_shader(convert_int_fsh frag)
dk3d_add_shader(downsample_fsh frag)
dk3d_add_shader(clut_fsh frag)
dk3d_add_shader(convert_8i_fsh frag)
dk3d_add_shader(date_fsh frag)
dk3d_add_shader(primid_init_fsh frag)
dk3d_add_shader(tfx_vsh vert)
dk3d_add_shader(tfx_fsh frag)
# Specialised tfx fragment variants
dk3d_add_tfx_variant(tfx_fsh_opaque TFX_VARIANT_OPAQUE)
dk3d_add_tfx_variant(tfx_fsh_palette TFX_VARIANT_PALETTE)
dk3d_add_tfx_variant(tfx_fsh_fast TFX_VARIANT_FAST)
dk3d_add_tfx_variant(tfx_fsh_feedback TFX_VARIANT_FEEDBACK)
dk3d_add_tfx_variant(tfx_fsh_feedback_palette TFX_VARIANT_FEEDBACK_PALETTE)
dk3d_add_tfx_variant(tfx_fsh_feedback_fast TFX_VARIANT_FEEDBACK_FAST)
dk3d_add_shader(interlace_fsh frag)
dk3d_add_shader(shadeboost_fsh frag)
dk3d_add_shader(fxaa_fsh frag)
dk3d_add_shader(merge_fsh frag)
dk3d_add_shader(yuv_fsh frag)
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

if(DK3D_RUNTIME_SHADERS)
	add_custom_command(
		OUTPUT "${DK3D_ROMFS_SHADER_DIR}/tfx_fsh.glsl"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different
			"${DK3D_SHADER_SRC_DIR}/tfx_fsh.glsl" "${DK3D_ROMFS_SHADER_DIR}/tfx_fsh.glsl"
		DEPENDS "${DK3D_SHADER_SRC_DIR}/tfx_fsh.glsl"
		COMMENT "Staging tfx_fsh.glsl for runtime compilation"
		VERBATIM
	)
	list(APPEND DK3D_DKSH_FILES "${DK3D_ROMFS_SHADER_DIR}/tfx_fsh.glsl")
endif()

add_custom_target(armsx2_shaders DEPENDS ${DK3D_DKSH_FILES})

# Bundle the runtime resources into the romfs.
set(ARMSX2_RESOURCES_SRC "${CMAKE_SOURCE_DIR}/../assets/resources")
set(ARMSX2_RESOURCES_DST "${DK3D_ROMFS_DIR}/resources")
file(GLOB_RECURSE ARMSX2_RESOURCE_FILES CONFIGURE_DEPENDS "${ARMSX2_RESOURCES_SRC}/*")

set(ARMSX2_STAGED_RESOURCES "")
foreach(_src IN LISTS ARMSX2_RESOURCE_FILES)
	file(RELATIVE_PATH _rel "${ARMSX2_RESOURCES_SRC}" "${_src}")
	set(_dst "${ARMSX2_RESOURCES_DST}/${_rel}")
	get_filename_component(_dstdir "${_dst}" DIRECTORY)
	add_custom_command(
		OUTPUT "${_dst}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${_dstdir}"
		COMMAND "${CMAKE_COMMAND}" -E copy "${_src}" "${_dst}"
		DEPENDS "${_src}"
		VERBATIM
	)
	list(APPEND ARMSX2_STAGED_RESOURCES "${_dst}")
endforeach()
add_custom_target(armsx2_resources DEPENDS ${ARMSX2_STAGED_RESOURCES})

dkp_add_asset_target(armsx2_romfs "${DK3D_ROMFS_DIR}")
add_dependencies(armsx2_romfs armsx2_shaders armsx2_resources)
set_property(TARGET armsx2_romfs APPEND PROPERTY DKP_ASSET_FILES
	${DK3D_DKSH_FILES} ${ARMSX2_STAGED_RESOURCES})

# Package an .nro
nx_generate_nacp(OUTPUT armsx2.nacp
	NAME "ARMSX2"
	AUTHOR "ARMSX2 Team \& PalindromicBreadLoaf"
	VERSION "3.0.0"
)
nx_create_nro(armsx2
	OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/armsx2nx.nro"
	NACP "${CMAKE_CURRENT_BINARY_DIR}/armsx2.nacp"
	ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg"
	ROMFS armsx2_romfs)
