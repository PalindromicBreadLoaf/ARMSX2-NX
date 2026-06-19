// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/DK3D/GSTextureDK.h"

#ifdef __SWITCH__
#include "common/Console.h"

#include <algorithm>
#include <cstring>

namespace
{
	constexpr u32 AlignUp(u32 value, u32 align)
	{
		return (value + align - 1) & ~(align - 1);
	}
} // namespace

DkImageFormat GSTextureDK::LookupFormat(Format format, bool& is_depth)
{
	is_depth = false;
	switch (format)
	{
		case Format::Color:        return DkImageFormat_RGBA8_Unorm;
		case Format::ColorHQ:      return DkImageFormat_RGB10A2_Unorm;
		case Format::ColorHDR:     return DkImageFormat_RGBA16_Float;
		case Format::ColorClip:    return DkImageFormat_RGBA16_Unorm;
		case Format::DepthStencil: is_depth = true; return DkImageFormat_ZF32_X24S8;
		case Format::UNorm8:       return DkImageFormat_R8_Unorm;
		case Format::UInt16:       return DkImageFormat_R16_Uint;
		case Format::UInt32:       return DkImageFormat_R32_Uint;
		case Format::PrimID:       return DkImageFormat_R32_Float;
		case Format::BC1:          return DkImageFormat_RGBA_BC1;
		case Format::BC2:          return DkImageFormat_RGBA_BC2;
		case Format::BC3:          return DkImageFormat_RGBA_BC3;
		case Format::BC7:          return DkImageFormat_RGBA_BC7_Unorm;
		default:                   return DkImageFormat_RGBA8_Unorm;
	}
}

std::unique_ptr<GSTextureDK> GSTextureDK::Create(DkDevice device, DkQueue upload_queue, Type type, Format format,
	int width, int height, int levels)
{
	bool is_depth = false;
	const DkImageFormat dk_format = LookupFormat(format, is_depth);

	DkImageLayoutMaker layout_maker;
	dkImageLayoutMakerDefaults(&layout_maker, device);
	layout_maker.format = dk_format;
	layout_maker.dimensions[0] = std::max(1, width);
	layout_maker.dimensions[1] = std::max(1, height);
	layout_maker.mipLevels = std::max(1, levels);

	switch (type)
	{
		case Type::RenderTarget:
		case Type::DepthStencil:
			layout_maker.flags = DkImageFlags_UsageRender;
			break;
		case Type::RWTexture:
			layout_maker.flags = DkImageFlags_UsageLoadStore;
			break;
		case Type::Texture:
		default:
			layout_maker.flags = 0;
			break;
	}

	DkImageLayout layout;
	dkImageLayoutInitialize(&layout, &layout_maker);

	const u32 image_size = static_cast<u32>(dkImageLayoutGetSize(&layout));
	const u32 image_align = dkImageLayoutGetAlignment(&layout);
	const u32 block_size = AlignUp(AlignUp(image_size, image_align), DK_MEMBLOCK_ALIGNMENT);

	DkMemBlockMaker memblock_maker;
	dkMemBlockMakerDefaults(&memblock_maker, device, block_size);
	memblock_maker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
	DkMemBlock memblock = dkMemBlockCreate(&memblock_maker);
	if (!memblock)
	{
		Console.Error("DK3D: failed to allocate %u bytes for a %dx%d texture.", block_size, width, height);
		return nullptr;
	}

	auto tex = std::unique_ptr<GSTextureDK>(
		new GSTextureDK(device, upload_queue, memblock, type, format, width, height, levels, dk_format, is_depth));

	dkImageInitialize(&tex->m_image, &layout, memblock, 0);

	// Build the sampler-side descriptor up front
	DkImageView view;
	dkImageViewDefaults(&view, &tex->m_image);
	dkImageDescriptorInitialize(&tex->m_descriptor, &view, false, false);
	return tex;
}

GSTextureDK::GSTextureDK(DkDevice device, DkQueue upload_queue, DkMemBlock memblock, Type type, Format format,
	int width, int height, int levels, DkImageFormat dk_format, bool is_depth)
	: m_device(device)
	, m_upload_queue(upload_queue)
	, m_memblock(memblock)
	, m_dk_format(dk_format)
	, m_is_depth(is_depth)
{
	m_type = type;
	m_format = format;
	m_size.x = std::max(1, width);
	m_size.y = std::max(1, height);
	m_mipmap_levels = std::max(1, levels);
}

GSTextureDK::~GSTextureDK()
{
	if (m_memblock)
		dkMemBlockDestroy(m_memblock);
}

void* GSTextureDK::GetNativeHandle() const
{
	return const_cast<DkImage*>(&m_image);
}

void GSTextureDK::GetImageView(DkImageView* view) const
{
	dkImageViewDefaults(view, &m_image);
}

bool GSTextureDK::Update(const GSVector4i& r, const void* data, int pitch, int layer)
{
	if (m_is_depth || !data)
		return false;

	const int width = r.z - r.x;
	const int height = r.w - r.y;
	if (width <= 0 || height <= 0)
		return false;

	// Compressed formats are uploaded in 4x4 blocks
	const u32 num_rows = IsCompressedFormat() ? ((static_cast<u32>(height) + 3) / 4) : static_cast<u32>(height);
	const u32 upload_pitch = CalcUploadPitch(static_cast<u32>(width));
	const u32 upload_size = num_rows * upload_pitch;

	DkMemBlockMaker memblock_maker;
	dkMemBlockMakerDefaults(&memblock_maker, m_device, AlignUp(upload_size, DK_MEMBLOCK_ALIGNMENT));
	memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	DkMemBlock staging = dkMemBlockCreate(&memblock_maker);
	if (!staging)
		return false;

	u8* dst = static_cast<u8*>(dkMemBlockGetCpuAddr(staging));
	const u8* src = static_cast<const u8*>(data);
	const u32 copy_bytes = std::min<u32>(upload_pitch, static_cast<u32>(pitch));
	for (u32 y = 0; y < num_rows; ++y)
		std::memcpy(dst + y * upload_pitch, src + y * static_cast<u32>(pitch), copy_bytes);

	dkMemBlockMakerDefaults(&memblock_maker, m_device, DK_MEMBLOCK_ALIGNMENT);
	memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
	DkMemBlock cmd_memblock = dkMemBlockCreate(&memblock_maker);
	if (!cmd_memblock)
	{
		dkMemBlockDestroy(staging);
		return false;
	}

	DkCmdBufMaker cmdbuf_maker;
	dkCmdBufMakerDefaults(&cmdbuf_maker, m_device);
	DkCmdBuf cmdbuf = dkCmdBufCreate(&cmdbuf_maker);
	dkCmdBufAddMemory(cmdbuf, cmd_memblock, 0, DK_MEMBLOCK_ALIGNMENT);

	DkImageView view;
	dkImageViewDefaults(&view, &m_image);
	view.mipLevelOffset = static_cast<uint8_t>(std::max(0, layer));
	view.mipLevelCount = 1;

	const DkCopyBuf copy_src = {dkMemBlockGetGpuAddr(staging), 0, 0};
	const DkImageRect copy_rect = {static_cast<u32>(r.x), static_cast<u32>(r.y), 0, static_cast<u32>(width),
		static_cast<u32>(height), 1};
	dkCmdBufCopyBufferToImage(cmdbuf, &copy_src, &view, &copy_rect, 0);

	dkQueueSubmitCommands(m_upload_queue, dkCmdBufFinishList(cmdbuf));
	dkQueueWaitIdle(m_upload_queue);

	dkCmdBufDestroy(cmdbuf);
	dkMemBlockDestroy(cmd_memblock);
	dkMemBlockDestroy(staging);

	m_state = State::Dirty;
	return true;
}

bool GSTextureDK::Map(GSMap& m, const GSVector4i* r, int layer)
{
	if (m_is_depth)
		return false;

	const GSVector4i area = r ? *r : GetRect();
	const int width = area.z - area.x;
	const int height = area.w - area.y;
	if (width <= 0 || height <= 0)
		return false;

	const u32 pitch = CalcUploadPitch(static_cast<u32>(width));
	m_map_buffer = std::make_unique<u8[]>(static_cast<size_t>(pitch) * static_cast<size_t>(height));
	m_map_area = area;
	m.bits = m_map_buffer.get();
	m.pitch = static_cast<int>(pitch);
	return true;
}

void GSTextureDK::Unmap()
{
	if (!m_map_buffer)
		return;

	const u32 pitch = CalcUploadPitch(static_cast<u32>(m_map_area.z - m_map_area.x));
	Update(m_map_area, m_map_buffer.get(), static_cast<int>(pitch), 0);
	m_map_buffer.reset();
}

void GSTextureDK::GenerateMipmap()
{
	// TODO: generate mips on the 2D engine.
}

#ifdef PCSX2_DEVBUILD
void GSTextureDK::SetDebugName(std::string_view name)
{
}
#endif
#endif
