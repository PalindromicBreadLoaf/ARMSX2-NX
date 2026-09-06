// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "HorizonUsbStorage.h"

#include <usbhsfs.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <utility>

namespace HorizonUsbStorage
{
namespace
{
	std::mutex s_volumes_mutex;
	std::vector<Volume> s_volumes;
	std::atomic_bool s_changed{false};
	bool s_initialized = false;
	std::string s_error;

	Volume ToVolume(const UsbHsFsDevice& device)
	{
		Volume volume;
		volume.root = std::string(device.name) + "/";
		volume.label = device.product_name[0] != '\0' ? device.product_name : device.name;
		volume.filesystem = LIBUSBHSFS_FS_TYPE_STR(device.fs_type);
		return volume;
	}

	void StoreVolumes(std::vector<Volume> volumes)
	{
		{
			const std::lock_guard lock(s_volumes_mutex);
			s_volumes = std::move(volumes);
		}
		s_changed.store(true, std::memory_order_release);
	}

	void PopulateCallback(const UsbHsFsDevice* devices, u32 device_count, void*)
	{
		std::vector<Volume> volumes;
		volumes.reserve(device_count);
		for (u32 i = 0; devices && i < device_count; i++)
			volumes.emplace_back(ToVolume(devices[i]));
		StoreVolumes(std::move(volumes));
	}

	void RefreshVolumes()
	{
		const u32 count = usbHsFsGetMountedDeviceCount();
		std::vector<UsbHsFsDevice> devices(count);
		const u32 listed = count ? usbHsFsListMountedDevices(devices.data(), count) : 0;

		std::vector<Volume> volumes;
		volumes.reserve(listed);
		for (u32 i = 0; i < listed; i++)
			volumes.emplace_back(ToVolume(devices[i]));
		StoreVolumes(std::move(volumes));
	}
} // namespace

bool Initialize()
{
	if (s_initialized)
		return true;

	usbHsFsSetFileSystemMountFlags(UsbHsFsMountFlags_ReadOnly);
	const Result result = usbHsFsInitialize(0);
	if (R_FAILED(result))
	{
		char message[96];
		std::snprintf(message, sizeof(message), "USB storage unavailable (0x%08X)", static_cast<unsigned>(result));
		s_error = message;
		return false;
	}

	s_initialized = true;
	s_error.clear();
	usbHsFsSetPopulateCallback(PopulateCallback, nullptr);
	RefreshVolumes();
	return true;
}

void Shutdown()
{
	if (!s_initialized)
		return;

	usbHsFsSetPopulateCallback(nullptr, nullptr);
	usbHsFsExit();
	s_initialized = false;
	StoreVolumes({});
	s_changed.store(false, std::memory_order_release);
}

bool IsAvailable()
{
	return s_initialized;
}

const std::string& GetError()
{
	return s_error;
}

std::vector<Volume> GetVolumes()
{
	const std::lock_guard lock(s_volumes_mutex);
	return s_volumes;
}

bool ConsumeChange()
{
	return s_changed.exchange(false, std::memory_order_acq_rel);
}
} // namespace HorizonUsbStorage
