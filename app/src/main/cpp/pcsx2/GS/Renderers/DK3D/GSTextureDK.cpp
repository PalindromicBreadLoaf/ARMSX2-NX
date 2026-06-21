// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/DK3D/GSTextureDK.h"

#ifdef __SWITCH__
#include "GS/Renderers/DK3D/GSDeviceDK.h"
#include "GS/GSPerfMon.h"

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

std::unique_ptr<GSTextureDK> GSTextureDK::Create(DkDevice device, GSDeviceDK* device_dk, Type type, Format format,
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
			layout_maker.flags = (std::max(1, levels) > 1 && !IsCompressedFormat(format))
									  ? (DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine)
									  : 0;
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
		new GSTextureDK(device, device_dk, memblock, type, format, width, height, levels, dk_format, is_depth));

	dkImageInitialize(&tex->m_image, &layout, memblock, 0);

	// Build the sampler-side descriptor up front
	DkImageView view;
	dkImageViewDefaults(&view, &tex->m_image);
	dkImageDescriptorInitialize(&tex->m_descriptor, &view, false, false);
	return tex;
}

GSTextureDK::GSTextureDK(DkDevice device, GSDeviceDK* device_dk, DkMemBlock memblock, Type type, Format format,
	int width, int height, int levels, DkImageFormat dk_format, bool is_depth)
	: m_device(device)
	, m_device_dk(device_dk)
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
	// ImGui uses this as its ImTextureID
	return const_cast<GSTextureDK*>(this);
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

	DkImageView view;
	dkImageViewDefaults(&view, &m_image);
	view.mipLevelOffset = static_cast<uint8_t>(std::max(0, layer));
	view.mipLevelCount = 1;

	const DkImageRect copy_rect = {static_cast<u32>(r.x), static_cast<u32>(r.y), 0, static_cast<u32>(width),
		static_cast<u32>(height), 1};

	// Stage into the device's per-frame ring and record the copy into the frame command buffer
	if (!m_device_dk->UploadToImage(view, copy_rect, data, static_cast<u32>(pitch), upload_pitch, num_rows))
		return false;

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
	if (m_is_depth || m_mipmap_levels <= 1)
		return;

	// Record the mip-chain downsample into the device's frame command buffer
	m_device_dk->GenerateImageMipmaps(&m_image, m_size.x, m_size.y, m_mipmap_levels);
}

#ifdef PCSX2_DEVBUILD
void GSTextureDK::SetDebugName(std::string_view name)
{
}
#endif

GSDownloadTextureDK::GSDownloadTextureDK(GSDeviceDK* device, DkMemBlock memblock, u32 buffer_size, u32 width,
	u32 height, GSTexture::Format format)
	: GSDownloadTexture(width, height, format)
	, m_device(device)
	, m_memblock(memblock)
	, m_buffer_size(buffer_size)
{
}

GSDownloadTextureDK::~GSDownloadTextureDK()
{
	if (m_memblock)
		dkMemBlockDestroy(m_memblock);
}

std::unique_ptr<GSDownloadTextureDK> GSDownloadTextureDK::Create(GSDeviceDK* device, DkDevice dk_device, u32 width,
	u32 height, GSTexture::Format format)
{
	const u32 buffer_size = GetBufferSize(width, height, format, 1);

	DkMemBlockMaker memblock_maker;
	dkMemBlockMakerDefaults(&memblock_maker, dk_device, AlignUp(buffer_size, DK_MEMBLOCK_ALIGNMENT));
	// CPU reads immediately after the copy
	memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuUncached;
	DkMemBlock memblock = dkMemBlockCreate(&memblock_maker);
	if (!memblock)
	{
		Console.Error("DK3D: failed to allocate %u byte readback buffer.", buffer_size);
		return nullptr;
	}

	auto tex = std::unique_ptr<GSDownloadTextureDK>(
		new GSDownloadTextureDK(device, memblock, buffer_size, width, height, format));
	tex->m_map_pointer = static_cast<const u8*>(dkMemBlockGetCpuAddr(memblock));
	return tex;
}

void GSDownloadTextureDK::CopyFromTexture(
	const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch)
{
	GSTextureDK* const dktex = static_cast<GSTextureDK*>(stex);
	if (dktex->GetFormat() != m_format || drc.width() != src.width() || drc.height() != src.height())
		return;

	m_current_pitch = GetTransferPitch(use_transfer_pitch ? static_cast<u32>(drc.width()) : m_width, 1);

	u32 copy_offset, copy_size, copy_rows;
	GetTransferSize(drc, &copy_offset, &copy_size, &copy_rows);

	g_perfmon.Put(GSPerfMon::Readbacks, 1);

	// Records the copy into the frame command buffer
	m_device->ReadbackTexture(dktex, src, m_memblock, copy_offset);
	m_needs_flush = true;
}

bool GSDownloadTextureDK::Map(const GSVector4i& read_rc)
{
	return (m_map_pointer != nullptr);
}

void GSDownloadTextureDK::Unmap()
{
}

void GSDownloadTextureDK::Flush()
{
	if (!m_needs_flush)
		return;

	m_needs_flush = false;
	// Block once for the queued copy instead of per copy.
	m_device->FlushReadback();
}

#ifdef PCSX2_DEVBUILD
void GSDownloadTextureDK::SetDebugName(std::string_view name)
{
}
#endif
#endif
