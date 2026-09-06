// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Horizon/Horizon.h"
#include "common/Path.h"
#include "common/ScopedGuard.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/Config.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SIO/Pad/PadDualshock2.h"
#include "pcsx2/VMManager.h"

#include "HorizonException.h"
#include "HorizonHost.h"
#include "HorizonUsbStorage.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <thread>

namespace
{
	constexpr const char* ARMSX2_ROOT = "sdmc:/switch/armsx2";
	constexpr const char* GAMES_DIR = "sdmc:/switch/armsx2/games";
	constexpr const char* LOGS_DIR = "sdmc:/switch/armsx2/logs";
	constexpr const char* SETTINGS_PATH = "sdmc:/switch/armsx2/armsx2.ini";
	constexpr u64 IDLE_POLL_NS = 8'000'000ULL;

	constexpr const char* USB_SETTINGS_SECTION = "Horizon";
	constexpr const char* USB_GAME_ROOTS_KEY = "ManagedUsbGameRoots";

	constexpr u64 INPUT_POLL_NS = 16'000'000ULL;
	constexpr float STICK_DEADZONE = 0.15f;
	constexpr u32 NUM_LOCAL_PLAYERS = 2;
	constexpr u64 MENU_COMBO = HidNpadButton_Plus | HidNpadButton_Minus;

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
			s_settings_interface->SetStringValue("Pad2", "Type", "DualShock2");
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

	bool SyncUsbGameRoots()
	{
		std::vector<std::string> roots;
		for (const HorizonUsbStorage::Volume& volume : HorizonUsbStorage::GetVolumes())
			roots.push_back(volume.root);

		auto lock = Host::GetSettingsLock();
		const std::vector<std::string> old_roots =
			s_settings_interface->GetStringList(USB_SETTINGS_SECTION, USB_GAME_ROOTS_KEY);
		if (roots == old_roots)
			return false;

		for (const std::string& root : old_roots)
			s_settings_interface->RemoveFromStringList("GameList", "RecursivePaths", root.c_str());
		for (const std::string& root : roots)
			s_settings_interface->AddToStringList("GameList", "RecursivePaths", root.c_str());

		s_settings_interface->SetStringList(USB_SETTINGS_SECTION, USB_GAME_ROOTS_KEY, roots);

		Error error;
		if (!s_settings_interface->Save(&error))
			ERROR_LOG("Failed to save USB game roots: {}", error.GetDescription());
		return true;
	}

	void LogUsbVolumes()
	{
		const std::vector<HorizonUsbStorage::Volume> volumes = HorizonUsbStorage::GetVolumes();
		if (volumes.empty())
		{
			INFO_LOG("USB storage disconnected");
			return;
		}

		for (const HorizonUsbStorage::Volume& volume : volumes)
			INFO_LOG("USB volume '{}' mounted at {} ({})", volume.label, volume.root, volume.filesystem);
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

	void ReportInitialBootFailure(const VMBootResult result, const Error& error)
	{
		const std::string message = error.IsValid() ? error.GetDescription() : "The virtual machine could not be started.";
		ERROR_LOG("Failed to boot launch image: {} (result {})", message, static_cast<int>(result));

		if (FullscreenUI::IsInitialized())
		{
			MTGS::RunOnGSThread([message]() {
				ImGuiFullscreen::OpenInfoMessageDialog("Startup Error", message);
			});
		}
		else
		{
			HorizonHost::RequestExit();
		}
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
		auto hardcore_disable_callback = [](std::string reason, VMBootRestartCallback restart_callback) {
			if (!FullscreenUI::IsInitialized())
			{
				ERROR_LOG("Launch image requires disabling RetroAchievements Hardcore Mode: {}", reason);
				HorizonHost::RequestExit();
				return;
			}

			MTGS::RunOnGSThread([reason = std::move(reason), restart_callback = std::move(restart_callback)]() mutable {
				ImGuiFullscreen::OpenConfirmMessageDialog(Achievements::GetHardcoreModeDisableTitle(),
					Achievements::GetHardcoreModeDisableText(reason.c_str()),
					[restart_callback = std::move(restart_callback)](bool confirmed) mutable {
						if (confirmed)
							Host::RunOnCPUThread(std::move(restart_callback));
					});
			});
		};
		auto done_callback = [](VMBootResult result, const Error& error) {
			if (result == VMBootResult::StartupSuccess)
				VMManager::SetState(VMState::Running);
			else
				ReportInitialBootFailure(result, error);
		};
		VMManager::InitializeAsync(boot_params, std::move(hardcore_disable_callback), std::move(done_callback));
		return true;
	}

	bool s_keyboard_shown_for_focus = false;

	void ProcessSoftwareKeyboard()
	{
		if (!ImGuiManager::WantsTextInput())
		{
			s_keyboard_shown_for_focus = false;
			return;
		}

		if (s_keyboard_shown_for_focus)
			return;
		s_keyboard_shown_for_focus = true;

		HorizonHost::SoftwareKeyboardParameters params;
		params.guide_text = "Enter text";
		if (std::optional<std::string> text = HorizonHost::ShowSoftwareKeyboard(params); text.has_value())
			ImGuiManager::AddTextInput(std::move(text.value()));
	}

	void RunMainLoop()
	{
		while (appletMainLoop())
		{
			Host::PumpMessagesOnCPUThread();
			ProcessAppletLifecycle();
			ProcessSoftwareKeyboard();

			if (HorizonUsbStorage::ConsumeChange())
			{
				LogUsbVolumes();
				if (SyncUsbGameRoots() && FullscreenUI::IsInitialized())
					Host::RefreshGameListAsync(false);
			}

			switch (VMManager::GetState())
			{
				case VMState::Running:
					VMManager::Execute();
					break;

				case VMState::Resetting:
					VMManager::Reset();
					break;

				case VMState::Stopping:
					VMManager::Shutdown(HorizonHost::TakeResumeSaveRequest());
					break;

				case VMState::Paused:
				case VMState::Shutdown:
					VMManager::IdlePollUpdate();
					svcSleepThread(IDLE_POLL_NS);
					break;

				case VMState::Initializing:
					break;
			}

			if (HorizonHost::IsExitRequested() && VMManager::GetState() == VMState::Shutdown)
				break;
		}

		HorizonHost::RequestExit();
	}

	struct ButtonMap
	{
		u64 nx;
		PadDualshock2::Inputs ps2;
	};
	constexpr ButtonMap BUTTON_MAP[] = {
		{HidNpadButton_Up, PadDualshock2::Inputs::PAD_UP},
		{HidNpadButton_Down, PadDualshock2::Inputs::PAD_DOWN},
		{HidNpadButton_Left, PadDualshock2::Inputs::PAD_LEFT},
		{HidNpadButton_Right, PadDualshock2::Inputs::PAD_RIGHT},
		{HidNpadButton_X, PadDualshock2::Inputs::PAD_TRIANGLE},
		{HidNpadButton_A, PadDualshock2::Inputs::PAD_CIRCLE},
		{HidNpadButton_B, PadDualshock2::Inputs::PAD_CROSS},
		{HidNpadButton_Y, PadDualshock2::Inputs::PAD_SQUARE},
		{HidNpadButton_Minus, PadDualshock2::Inputs::PAD_SELECT},
		{HidNpadButton_Plus, PadDualshock2::Inputs::PAD_START},
		{HidNpadButton_L, PadDualshock2::Inputs::PAD_L1},
		{HidNpadButton_R, PadDualshock2::Inputs::PAD_R1},
		{HidNpadButton_ZL, PadDualshock2::Inputs::PAD_L2},
		{HidNpadButton_ZR, PadDualshock2::Inputs::PAD_R2},
		{HidNpadButton_StickL, PadDualshock2::Inputs::PAD_L3},
		{HidNpadButton_StickR, PadDualshock2::Inputs::PAD_R3},
	};

	struct NavMap
	{
		u64 nx;
		GenericInputBinding generic;
	};
	constexpr NavMap NAV_MAP[] = {
		{HidNpadButton_Up, GenericInputBinding::DPadUp},
		{HidNpadButton_Down, GenericInputBinding::DPadDown},
		{HidNpadButton_Left, GenericInputBinding::DPadLeft},
		{HidNpadButton_Right, GenericInputBinding::DPadRight},
		{HidNpadButton_B, GenericInputBinding::Cross},
		{HidNpadButton_A, GenericInputBinding::Circle},
		{HidNpadButton_Y, GenericInputBinding::Square},
		{HidNpadButton_X, GenericInputBinding::Triangle},
		{HidNpadButton_L, GenericInputBinding::L1},
		{HidNpadButton_R, GenericInputBinding::R1},
		{HidNpadButton_ZL, GenericInputBinding::L2},
		{HidNpadButton_ZR, GenericInputBinding::R2},
		{HidNpadButton_Minus, GenericInputBinding::Select},
		{HidNpadButton_Plus, GenericInputBinding::Start},
	};

	std::array<PadState, NUM_LOCAL_PLAYERS> s_pads;
	std::thread s_input_thread;
	std::atomic_bool s_input_stop{false};

	float ApplyDeadzone(s32 raw)
	{
		const float v = std::clamp(static_cast<float>(raw) / static_cast<float>(JOYSTICK_MAX), -1.0f, 1.0f);
		const float mag = std::fabs(v);
		if (mag < STICK_DEADZONE)
			return 0.0f;
		const float scaled = (mag - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
		return (v < 0.0f) ? -scaled : scaled;
	}

	void ApplyStick(u32 player, const HidAnalogStickState& s, PadDualshock2::Inputs left, PadDualshock2::Inputs right,
		PadDualshock2::Inputs up, PadDualshock2::Inputs down)
	{
		const float x = ApplyDeadzone(s.x);
		const float y = ApplyDeadzone(s.y);
		Pad::SetControllerState(player, static_cast<u32>(right), x > 0.0f ? x : 0.0f);
		Pad::SetControllerState(player, static_cast<u32>(left), x < 0.0f ? -x : 0.0f);
		Pad::SetControllerState(player, static_cast<u32>(up), y > 0.0f ? y : 0.0f);
		Pad::SetControllerState(player, static_cast<u32>(down), y < 0.0f ? -y : 0.0f);
	}

	void FeedGamePad(u32 player, PadState& pad, u64 held)
	{
		for (const ButtonMap& m : BUTTON_MAP)
			Pad::SetControllerState(player, static_cast<u32>(m.ps2), (held & m.nx) ? 1.0f : 0.0f);

		ApplyStick(player, padGetStickPos(&pad, 0), PadDualshock2::Inputs::PAD_L_LEFT, PadDualshock2::Inputs::PAD_L_RIGHT,
			PadDualshock2::Inputs::PAD_L_UP, PadDualshock2::Inputs::PAD_L_DOWN);
		ApplyStick(player, padGetStickPos(&pad, 1), PadDualshock2::Inputs::PAD_R_LEFT, PadDualshock2::Inputs::PAD_R_RIGHT,
			PadDualshock2::Inputs::PAD_R_UP, PadDualshock2::Inputs::PAD_R_DOWN);
	}

	void FeedNav(u64 held, u64 changed)
	{
		for (const NavMap& m : NAV_MAP)
		{
			if (changed & m.nx)
				ImGuiManager::ProcessGenericInputEvent(m.generic, InputLayout::Nintendo, (held & m.nx) ? 1.0f : 0.0f);
		}
	}

	void InputPollLoop()
	{
		std::array<u64, NUM_LOCAL_PLAYERS> prev_held{};
		bool prev_menu_combo = false;
		while (!s_input_stop.load(std::memory_order_relaxed))
		{
			std::array<u64, NUM_LOCAL_PLAYERS> held{};
			std::array<u64, NUM_LOCAL_PLAYERS> changed{};
			for (u32 player = 0; player < NUM_LOCAL_PLAYERS; player++)
			{
				padUpdate(&s_pads[player]);
				held[player] = padGetButtons(&s_pads[player]);
				changed[player] = held[player] ^ prev_held[player];
				prev_held[player] = held[player];
			}

			if (FullscreenUI::HasActiveWindow())
				FeedNav(held[0], changed[0]);
			else if (VMManager::HasValidVM())
			{
				for (u32 player = 0; player < NUM_LOCAL_PLAYERS; player++)
					FeedGamePad(player, s_pads[player], held[player]);
			}

			const bool menu_combo = (held[0] & MENU_COMBO) == MENU_COMBO;
			if (menu_combo && !prev_menu_combo)
			{
				if (FullscreenUI::IsInitialized() && VMManager::HasValidVM())
					FullscreenUI::OpenPauseMenu();
				else if (!FullscreenUI::IsInitialized())
					HorizonHost::RequestExit();
			}
			prev_menu_combo = menu_combo;

			svcSleepThread(INPUT_POLL_NS);
		}
	}

	void StartInputPolling()
	{
		padConfigureInput(NUM_LOCAL_PLAYERS, HidNpadStyleSet_NpadStandard);
		padInitializeDefault(&s_pads[0]);
		padInitialize(&s_pads[1], HidNpadIdType_No2);

		s_input_stop.store(false, std::memory_order_relaxed);
		s_input_thread = std::thread(InputPollLoop);
	}

	void StopInputPolling()
	{
		if (!s_input_thread.joinable())
			return;
		s_input_stop.store(true, std::memory_order_relaxed);
		s_input_thread.join();
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

	HorizonException::Initialize(LOGS_DIR);

	Log::SetTimestampsEnabled(true);
	VMManager::Internal::SetFileLogPath(Path::Combine(LOGS_DIR, "emulog.txt"));

	const bool network_available = R_SUCCEEDED(socketInitializeDefault());
	if (!network_available)
		WARNING_LOG("socketInitializeDefault() failed");
	const bool ssl_available = network_available && R_SUCCEEDED(sslInitialize(4));
	if (network_available && !ssl_available)
		WARNING_LOG("sslInitialize() failed");
	const ScopedGuard network_guard([network_available, ssl_available]() {
		if (ssl_available)
			sslExit();
		if (network_available)
			socketExit();
	});

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

	if (HorizonUsbStorage::Initialize())
	{
		SyncUsbGameRoots();
		LogUsbVolumes();
		HorizonUsbStorage::ConsumeChange();
	}
	else
	{
		SyncUsbGameRoots();
		WARNING_LOG("{}", HorizonUsbStorage::GetError());
	}

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

	if (!HorizonHost::IsExitRequested() && argc > 1 && !BootInitialImage(argv[1]) && !FullscreenUI::IsInitialized())
		HorizonHost::RequestExit();

	StartInputPolling();

	RunMainLoop();

	StopInputPolling();

	appletUnhook(&applet_hook);
	Host::CancelGameListRefresh();
	HorizonUsbStorage::Shutdown();
	if (VMManager::GetState() != VMState::Shutdown)
		VMManager::Shutdown(HorizonHost::TakeResumeSaveRequest());
	if (MTGS::IsOpen())
		MTGS::WaitForClose();
	VMManager::Internal::CPUThreadShutdown();

	if (romfs_available)
		romfsExit();
	HorizonException::Shutdown();
	appletUnlockExit();
	return 0;
}
