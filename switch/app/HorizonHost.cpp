// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "HorizonHost.h"

#include "common/Console.h"
#include "common/ProgressCallback.h"
#include "common/Threading.h"
#include "common/WindowInfo.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/GameList.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/VMManager.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace
{
	std::mutex s_cpu_queue_mutex;
	std::deque<std::function<void()>> s_cpu_queue;
	std::condition_variable s_cpu_queue_cv;

	std::thread::id s_cpu_thread_id;
	std::atomic_bool s_cpu_thread_valid{false};
	std::atomic_bool s_exit_requested{false};

	std::mutex s_gamelist_refresh_mutex;
	std::thread s_gamelist_refresh_thread;
} // namespace

void HorizonHost::SetCPUThread()
{
	s_cpu_thread_id = std::this_thread::get_id();
	s_cpu_thread_valid.store(true, std::memory_order_release);
}

bool HorizonHost::IsCPUThread()
{
	return s_cpu_thread_valid.load(std::memory_order_acquire) && std::this_thread::get_id() == s_cpu_thread_id;
}

void HorizonHost::RequestExit()
{
	s_exit_requested.store(true, std::memory_order_release);
	s_cpu_queue_cv.notify_all();

	if (VMManager::HasValidVM())
	{
		const VMState state = VMManager::GetState();
		if (state == VMState::Running || state == VMState::Paused)
			VMManager::SetState(VMState::Stopping);
	}
}

bool HorizonHost::IsExitRequested()
{
	return s_exit_requested.load(std::memory_order_acquire);
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	return std::nullopt;
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

bool Host::IsFullscreen()
{
	return true;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
	si.SetBoolValue("UI", "StartBigPictureMode", true);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		ERROR_LOG("{}: {}", title, message);
	else if (!message.empty())
		ERROR_LOG("{}", message);
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		INFO_LOG("{}: {}", title, message);
	else if (!message.empty())
		INFO_LOG("{}", message);
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

void Host::OpenURL(const std::string_view url)
{
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

std::string Host::GetTextFromClipboard()
{
	return {};
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	return lhs.compare(rhs);
}

void Host::BeginTextInput()
{
}

void Host::EndTextInput()
{
}

bool Host::LocaleCircleConfirm()
{
	return false;
}

bool Host::InBatchMode()
{
	return false;
}

bool Host::InNoGUIMode()
{
	return false;
}

void Host::OnVMStarting()
{
	INFO_LOG("Host: VM starting");
}

void Host::OnVMStarted()
{
	INFO_LOG("Host: VM started");
}

void Host::OnVMDestroyed()
{
	INFO_LOG("Host: VM destroyed");
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
	INFO_LOG("Host: game changed - title='{}' serial='{}' crc={:08X}", title, disc_serial, current_crc);
}

void Host::OnPerformanceMetricsUpdated()
{
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

void Host::PumpMessagesOnCPUThread()
{
	for (;;)
	{
		std::function<void()> task;
		{
			std::lock_guard<std::mutex> lock(s_cpu_queue_mutex);
			if (s_cpu_queue.empty())
				break;

			task = std::move(s_cpu_queue.front());
			s_cpu_queue.pop_front();
		}
		task();
	}
}

void Host::RunOnCPUThread(std::function<void()> function, bool block)
{
	if (HorizonHost::IsCPUThread())
	{
		function();
		return;
	}

	if (!block)
	{
		std::lock_guard<std::mutex> lock(s_cpu_queue_mutex);
		s_cpu_queue.push_back(std::move(function));
		s_cpu_queue_cv.notify_one();
		return;
	}

	std::mutex completed_mutex;
	std::condition_variable completed_cv;
	bool completed = false;
	{
		std::lock_guard<std::mutex> lock(s_cpu_queue_mutex);
		s_cpu_queue.emplace_back([function = std::move(function), &completed_mutex, &completed_cv, &completed]() mutable {
			function();
			{
				std::lock_guard<std::mutex> completed_lock(completed_mutex);
				completed = true;
			}
			completed_cv.notify_one();
		});
	}
	s_cpu_queue_cv.notify_one();

	std::unique_lock<std::mutex> completed_lock(completed_mutex);
	completed_cv.wait(completed_lock, [&completed]() { return completed; });
}

void Host::RunOnGSThread(std::function<void()> function)
{
	RunOnCPUThread([function = std::move(function)]() mutable { MTGS::RunOnGSThread(std::move(function)); });
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
	std::lock_guard<std::mutex> lock(s_gamelist_refresh_mutex);
	if (s_gamelist_refresh_thread.joinable())
		s_gamelist_refresh_thread.join();

	s_gamelist_refresh_thread = std::thread([invalidate_cache]() {
		Threading::SetNameOfCurrentThread("GameList Refresh");
		GameList::Refresh(invalidate_cache, false, nullptr);
	});
}

void Host::CancelGameListRefresh()
{
	std::lock_guard<std::mutex> lock(s_gamelist_refresh_mutex);
	if (s_gamelist_refresh_thread.joinable())
		s_gamelist_refresh_thread.join();
}

void Host::RequestExitApplication(bool allow_confirm)
{
	HorizonHost::RequestExit();
}

void Host::RequestExitBigPicture()
{
	HorizonHost::RequestExit();
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	if (VMManager::HasValidVM())
		VMManager::SetState(VMState::Stopping);
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
	INFO_LOG("Input device connected: {} ({})", identifier, device_name);
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
	INFO_LOG("Input device disconnected: {}", identifier);
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::SetMouseLock(bool state)
{
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

bool Host::HasNativeAchievementNotifications()
{
	return false;
}

void Host::OnAchievementNotification(const char* key, float duration, const char* title, const char* message,
	const char* badge_path)
{
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
}

void Host::OnAchievementsRefreshed()
{
}

void Host::OnCoverDownloaderOpenRequested()
{
}

void Host::OnCreateMemoryCardOpenRequested()
{
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	callback({});
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	return std::string(msg);
}

BEGIN_HOTKEY_LIST(g_common_hotkeys)
END_HOTKEY_LIST()

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

void VMManager::Internal::ResetVMHotkeyState()
{
}
