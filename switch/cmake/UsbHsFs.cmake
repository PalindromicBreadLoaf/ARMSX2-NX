# libusbhsfs mounts USB mass-storage partitions as devoptab paths (ums0:/, ums1:/, ...).

set(USBHSFS_SRC "${CMAKE_CURRENT_LIST_DIR}/../externals/libusbhsfs")

if(NOT EXISTS "${USBHSFS_SRC}/include/usbhsfs.h")
	message(FATAL_ERROR
		"libusbhsfs submodule is missing. Run: git submodule update --init --recursive")
endif()

find_path(USBHSFS_NTFS_3G_INCLUDE_DIR ntfs-3g/volume.h)
find_library(USBHSFS_NTFS_3G ntfs-3g)
find_path(USBHSFS_LWEXT4_INCLUDE_DIR ext4.h)
find_library(USBHSFS_LWEXT4 lwext4)

if(NOT USBHSFS_NTFS_3G OR NOT USBHSFS_NTFS_3G_INCLUDE_DIR OR
	NOT USBHSFS_LWEXT4 OR NOT USBHSFS_LWEXT4_INCLUDE_DIR)
	message(FATAL_ERROR
		"USB storage requires switch-ntfs-3g and switch-lwext4 for FAT/exFAT/NTFS/EXT2/3/4 support. "
		"Please install both and reconfigure.")
endif()

set(USBHSFS_SOURCES
	${USBHSFS_SRC}/source/usbhsfs_drive.c
	${USBHSFS_SRC}/source/usbhsfs_log.c
	${USBHSFS_SRC}/source/usbhsfs_manager.c
	${USBHSFS_SRC}/source/usbhsfs_mount.c
	${USBHSFS_SRC}/source/usbhsfs_request.c
	${USBHSFS_SRC}/source/usbhsfs_scsi.c
	${USBHSFS_SRC}/source/usbhsfs_utils.c
	${USBHSFS_SRC}/source/fatfs/diskio.c
	${USBHSFS_SRC}/source/fatfs/ff.c
	${USBHSFS_SRC}/source/fatfs/ff_dev.c
	${USBHSFS_SRC}/source/fatfs/ffsystem.c
	${USBHSFS_SRC}/source/fatfs/ffunicode.c
	${USBHSFS_SRC}/source/sxos/usbfs.c
	${USBHSFS_SRC}/source/sxos/usbfs_dev.c
	${USBHSFS_SRC}/source/ntfs-3g/ntfs.c
	${USBHSFS_SRC}/source/ntfs-3g/ntfs_dev.c
	${USBHSFS_SRC}/source/ntfs-3g/ntfs_disk_io.c
	${USBHSFS_SRC}/source/lwext4/ext.c
	${USBHSFS_SRC}/source/lwext4/ext_dev.c
	${USBHSFS_SRC}/source/lwext4/ext_disk_io.c
)

add_library(usbhsfs STATIC ${USBHSFS_SOURCES})
string(TIMESTAMP USBHSFS_BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S UTC" UTC)
target_compile_definitions(usbhsfs PRIVATE
	LIB_TITLE="libusbhsfs"
	BUILD_TIMESTAMP="${USBHSFS_BUILD_TIMESTAMP}"
	_GNU_SOURCE
	GPL_BUILD
)
target_compile_options(usbhsfs PRIVATE -w)
target_include_directories(usbhsfs
	PUBLIC ${USBHSFS_SRC}/include
	PRIVATE ${USBHSFS_SRC}/source ${USBHSFS_NTFS_3G_INCLUDE_DIR} ${USBHSFS_LWEXT4_INCLUDE_DIR}
)
target_link_libraries(usbhsfs PUBLIC ${USBHSFS_NTFS_3G} ${USBHSFS_LWEXT4})

message(STATUS "libusbhsfs: FAT, exFAT, NTFS and EXT2/3/4 USB storage support")
