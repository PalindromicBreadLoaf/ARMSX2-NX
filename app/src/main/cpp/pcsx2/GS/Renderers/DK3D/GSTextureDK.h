// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSTexture.h"

#ifdef __SWITCH__
#include <deko3d.h>
#include <memory>

class GSDeviceDK;

// Deko3D backend for GSTexture
class GSTextureDK final : public GSTexture
{
public:
	~GSTextureDK() override;

	static std::unique_ptr<GSTextureDK> Create(DkDevice device, GSDeviceDK* device_dk, Type type, Format format,
		int width, int height, int levels);

	void* GetNativeHandle() const override;
	bool Update(const GSVector4i& r, const void* data, int pitch, int layer = 0) override;
	bool Map(GSMap& m, const GSVector4i* r = nullptr, int layer = 0) override;
	void Unmap() override;
	void GenerateMipmap() override;

#ifdef PCSX2_DEVBUILD
	void SetDebugName(std::string_view name) override;
#endif

	__fi DkImage* GetImage() { return &m_image; }
	__fi const DkImageDescriptor& GetDescriptor() const { return m_descriptor; }
	__fi DkImageFormat GetDkFormat() const { return m_dk_format; }
	__fi bool IsDepth() const { return m_is_depth; }
	void GetImageView(DkImageView* view) const;

	static DkImageFormat LookupFormat(Format format, bool& is_depth);

private:
	GSTextureDK(DkDevice device, GSDeviceDK* device_dk, DkMemBlock memblock, Type type, Format format, int width,
		int height, int levels, DkImageFormat dk_format, bool is_depth);

	DkDevice m_device = nullptr;
	GSDeviceDK* m_device_dk = nullptr;
	DkMemBlock m_memblock = nullptr;
	DkImage m_image{};
	DkImageDescriptor m_descriptor{};
	DkImageFormat m_dk_format = DkImageFormat_None;
	bool m_is_depth = false;

	std::unique_ptr<u8[]> m_map_buffer;
	GSVector4i m_map_area = GSVector4i::zero();
};

class GSDeviceDK;

// Deko3D render-target/texture readback
class GSDownloadTextureDK final : public GSDownloadTexture
{
public:
	~GSDownloadTextureDK() override;

	static std::unique_ptr<GSDownloadTextureDK> Create(GSDeviceDK* device, DkDevice dk_device, u32 width, u32 height,
		GSTexture::Format format);

	void CopyFromTexture(const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level,
		bool use_transfer_pitch) override;
	bool Map(const GSVector4i& read_rc) override;
	void Unmap() override;
	void Flush() override;

#ifdef PCSX2_DEVBUILD
	void SetDebugName(std::string_view name) override;
#endif

private:
	GSDownloadTextureDK(GSDeviceDK* device, DkMemBlock memblock, u32 buffer_size, u32 width, u32 height,
		GSTexture::Format format);

	GSDeviceDK* m_device = nullptr;
	DkMemBlock m_memblock = nullptr;
	u32 m_buffer_size = 0;
};
#endif
