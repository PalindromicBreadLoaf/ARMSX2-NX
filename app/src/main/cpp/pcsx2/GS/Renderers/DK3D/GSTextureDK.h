// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSTexture.h"

#ifdef __SWITCH__
#include <deko3d.h>
#include <memory>

// Deko3D backend for GSTexture
class GSTextureDK final : public GSTexture
{
public:
	~GSTextureDK() override;

	static std::unique_ptr<GSTextureDK> Create(DkDevice device, DkQueue upload_queue, Type type, Format format,
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
	GSTextureDK(DkDevice device, DkQueue upload_queue, DkMemBlock memblock, Type type, Format format, int width,
		int height, int levels, DkImageFormat dk_format, bool is_depth);

	DkDevice m_device = nullptr;
	DkQueue m_upload_queue = nullptr;
	DkMemBlock m_memblock = nullptr;
	DkImage m_image{};
	DkImageDescriptor m_descriptor{};
	DkImageFormat m_dk_format = DkImageFormat_None;
	bool m_is_depth = false;

	std::unique_ptr<u8[]> m_map_buffer;
	GSVector4i m_map_area = GSVector4i::zero();
};
#endif
