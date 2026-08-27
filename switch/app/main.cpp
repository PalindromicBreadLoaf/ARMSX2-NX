// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Horizon/Horizon.h"
#include "common/Path.h"

#include "pcsx2/Config.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/VMManager.h"

#include "HorizonHost.h"

#include <atomic>
#include <memory>
#include <string>
#include <sys/stat.h>

namespace
{
	constexpr const char* ARMSX2_ROOT = "sdmc:/switch/armsx2";
	constexpr const char* GAMES_DIR = "sdmc:/switch/armsx2/games";
	constexpr const char* LOGS_DIR = "sdmc:/switch/armsx2/logs";
	constexpr const char* SETTINGS_PATH = "sdmc:/switch/armsx2/armsx2.ini";
	constexpr u64 IDLE_POLL_NS = 8'000'000ULL;

	std::unique_ptr<INISettingsInterface> s_settings_interface;
	std::atomic_bool s_applet_backgrounded{false};
	std::atomic_bool s_applet_resumed{false};
	bool s_paused_for_applet = false;

	void AppletHookCallback(AppletHookType hook, void*)
	{
		switch (hook)
		{
			case AppletHookType_OnExitRequest:
				HorizonHost::RequestExit();
				break;

			case AppletHookType_OnFocusState:
				if (appletGetFocusState() == AppletFocusState_Background)
					s_applet_backgrounded.store(true, std::memory_order_release);
				break;

			case AppletHookType_OnResume:
				s_applet_resumed.store(true, std::memory_order_release);
				break;

			default:
				break;
		}
	}

	void ProcessAppletLifecycle()
	{
		if (s_applet_backgrounded.exchange(false, std::memory_order_acq_rel))
		{
			if (VMManager::GetState() == VMState::Running)
			{
				VMManager::SetPaused(true);
				s_paused_for_applet = true;
			}
		}

		if (s_applet_resumed.exchange(false, std::memory_order_acq_rel) && s_paused_for_applet)
		{
			if (VMManager::GetState() == VMState::Paused)
				VMManager::SetPaused(false);
			s_paused_for_applet = false;
		}
	}

	bool InitializeSettings()
	{
		s_settings_interface = std::make_unique<INISettingsInterface>(SETTINGS_PATH);
		Host::Internal::SetBaseSettingsLayer(s_settings_interface.get());

		if (!s_settings_interface->Load() || !VMManager::Internal::CheckSettingsVersion())
		{
			INFO_LOG("Initializing Switch settings at {}", SETTINGS_PATH);
			VMManager::SetDefaultSettings(*s_settings_interface, true, true, true, true, true);
			s_settings_interface->SetBoolValue("UI", "EnableFullscreenUI", true);
			s_settings_interface->AddToStringList("GameList", "RecursivePaths", GAMES_DIR);
		}

		Error error;
		if (!s_settings_interface->Save(&error))
		{
			ERROR_LOG("Failed to save settings: {}", error.GetDescription());
			return false;
		}

		VMManager::Internal::LoadStartupSettings();
		return true;
	}

	bool InitializeFullscreenUI()
	{
		if (!ImGuiManager::InitializeFullscreenUI())
		{
			ERROR_LOG("Failed to initialize FullscreenUI");
			return false;
		}

		if (!MTGS::WaitForOpen())
		{
			ERROR_LOG("Failed to open the GS thread for FullscreenUI");
			return false;
		}

		MTGS::RunOnGSThread([]() { MTGS::SetRunIdle(true); });
		Host::RefreshGameListAsync(false);
		return true;
	}

	bool BootInitialImage(const char* image_path)
	{
		if (!image_path || image_path[0] == '\0')
			return true;

		if (!FileSystem::FileExists(image_path))
		{
			ERROR_LOG("Launch image not found: {}", image_path);
			return false;
		}

		VMBootParameters boot_params;
		boot_params.filename = image_path;
		const VMBootResult result = VMManager::Initialize(boot_params);
		if (result != VMBootResult::StartupSuccess)
		{
			ERROR_LOG("Failed to boot '{}': VM initialization result {}", image_path, static_cast<int>(result));
			return false;
		}

		VMManager::SetState(VMState::Running);
		return true;
	}

	void RunMainLoop()
	{
		while (!HorizonHost::IsExitRequested() && appletMainLoop())
		{
			Host::PumpMessagesOnCPUThread();
			ProcessAppletLifecycle();

			switch (VMManager::GetState())
			{
				case VMState::Running:
					VMManager::Execute();
					break;

				case VMState::Resetting:
					VMManager::Reset();
					break;

				case VMState::Stopping:
					VMManager::Shutdown(false);
					break;

				case VMState::Paused:
				case VMState::Shutdown:
					VMManager::IdlePollUpdate();
					svcSleepThread(IDLE_POLL_NS);
					break;

				case VMState::Initializing:
					break;
			}
		}

		HorizonHost::RequestExit();
	}
} // namespace

void Host::CommitBaseSettingChanges()
{
	if (!s_settings_interface)
		return;

	Error error;
	if (!s_settings_interface->Save(&error))
		ERROR_LOG("Failed to save settings: {}", error.GetDescription());
}

int main(int argc, char* argv[])
{
	appletLockExit();

	mkdir("sdmc:/switch", 0777);
	mkdir(ARMSX2_ROOT, 0777);
	mkdir(GAMES_DIR, 0777);
	mkdir(LOGS_DIR, 0777);

	Log::SetTimestampsEnabled(true);
	VMManager::Internal::SetFileLogPath(Path::Combine(LOGS_DIR, "emulog.txt"));

	const bool romfs_available = R_SUCCEEDED(romfsInit());
	EmuFolders::AppRoot = ARMSX2_ROOT;
	EmuFolders::DataRoot = ARMSX2_ROOT;
	EmuFolders::Settings = Path::Combine(ARMSX2_ROOT, "inis");
	if (romfs_available)
		EmuFolders::Resources = "romfs:/resources";
	else if (!EmuFolders::SetResourcesDirectory())
	{
		ERROR_LOG("ROMFS is unavailable and no SD resources directory was found");
		appletUnlockExit();
		return 1;
	}

	if (!InitializeSettings())
	{
		ERROR_LOG("Failed to initialize Switch settings");
		if (romfs_available)
			romfsExit();
		appletUnlockExit();
		return 1;
	}

	if (!VMManager::Internal::CPUThreadInitialize())
	{
		ERROR_LOG("Failed to initialize the Switch frontend");
		VMManager::Internal::CPUThreadShutdown();
		if (romfs_available)
			romfsExit();
		appletUnlockExit();
		return 1;
	}

	HorizonHost::SetCPUThread();
	VMManager::ApplySettings();

	AppletHookCookie applet_hook{};
	appletHook(&applet_hook, AppletHookCallback, nullptr);

	const bool fullscreen_ui = Host::GetBaseBoolSettingValue("UI", "EnableFullscreenUI", true);
	if (fullscreen_ui && !InitializeFullscreenUI())
		HorizonHost::RequestExit();
	else if (!fullscreen_ui && argc < 2)
	{
		ERROR_LOG("FullscreenUI is disabled and no launch image was provided");
		HorizonHost::RequestExit();
	}

	if (!HorizonHost::IsExitRequested() && argc > 1)
		BootInitialImage(argv[1]);

	RunMainLoop();

	appletUnhook(&applet_hook);
	Host::CancelGameListRefresh();
	if (VMManager::HasValidVM())
		VMManager::Shutdown(false);
	if (MTGS::IsOpen())
		MTGS::WaitForClose();
	VMManager::Internal::CPUThreadShutdown();

	if (romfs_available)
		romfsExit();
	appletUnlockExit();
	return 0;
}
