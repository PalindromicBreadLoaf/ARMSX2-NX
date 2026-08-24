# Copyright(c) 2026: SirSamael
# SPDX-License-Identifier: GPL-2.0+

set(_horizon_zstd_dir "${CMAKE_SOURCE_DIR}/platforms/android/app/src/main/cpp/3rdparty/zstd/lib")
set(_horizon_zstd_sources
	"${_horizon_zstd_dir}/common/debug.c"
	"${_horizon_zstd_dir}/common/entropy_common.c"
	"${_horizon_zstd_dir}/common/error_private.c"
	"${_horizon_zstd_dir}/common/fse_decompress.c"
	"${_horizon_zstd_dir}/common/pool.c"
	"${_horizon_zstd_dir}/common/threading.c"
	"${_horizon_zstd_dir}/common/xxhash.c"
	"${_horizon_zstd_dir}/common/zstd_common.c"
	"${_horizon_zstd_dir}/compress/fse_compress.c"
	"${_horizon_zstd_dir}/compress/hist.c"
	"${_horizon_zstd_dir}/compress/huf_compress.c"
	"${_horizon_zstd_dir}/compress/zstd_compress.c"
	"${_horizon_zstd_dir}/compress/zstd_compress_literals.c"
	"${_horizon_zstd_dir}/compress/zstd_compress_sequences.c"
	"${_horizon_zstd_dir}/compress/zstd_compress_superblock.c"
	"${_horizon_zstd_dir}/compress/zstd_double_fast.c"
	"${_horizon_zstd_dir}/compress/zstd_fast.c"
	"${_horizon_zstd_dir}/compress/zstd_lazy.c"
	"${_horizon_zstd_dir}/compress/zstd_ldm.c"
	"${_horizon_zstd_dir}/compress/zstd_opt.c"
	"${_horizon_zstd_dir}/compress/zstd_preSplit.c"
	"${_horizon_zstd_dir}/compress/zstdmt_compress.c"
	"${_horizon_zstd_dir}/decompress/huf_decompress.c"
	"${_horizon_zstd_dir}/decompress/zstd_ddict.c"
	"${_horizon_zstd_dir}/decompress/zstd_decompress.c"
	"${_horizon_zstd_dir}/decompress/zstd_decompress_block.c"
	"${_horizon_zstd_dir}/dictBuilder/cover.c"
	"${_horizon_zstd_dir}/dictBuilder/divsufsort.c"
	"${_horizon_zstd_dir}/dictBuilder/fastcover.c"
	"${_horizon_zstd_dir}/dictBuilder/zdict.c"
)

add_library(libzstd_static STATIC ${_horizon_zstd_sources})
add_library(Zstd::Zstd ALIAS libzstd_static)
target_include_directories(libzstd_static PUBLIC "${_horizon_zstd_dir}")
target_compile_definitions(libzstd_static PRIVATE ZSTD_DISABLE_ASM)

unset(_horizon_zstd_dir)
unset(_horizon_zstd_sources)
