// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "pcsx2/Config.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SIO/Pad/PadDualshock2.h"
#include "pcsx2/VMManager.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>

#include "common/Horizon/Horizon.h"

#include "HorizonHost.h"

namespace
{
	constexpr const char* ARMSX2_ROOT = "sdmc:/switch/armsx2";
	constexpr const char* GAMES_DIR = "sdmc:/switch/armsx2/games";
	constexpr const char* LOG_PATH = "sdmc:/switch/armsx2/armsx2.log";

	constexpr u64 INPUT_POLL_NS = 16'000'000ULL;
	// CPU-thread idle tick while no VM is running
	constexpr u64 IDLE_POLL_NS = 8'000'000ULL;

	constexpr float STICK_DEADZONE = 0.15f;

	// Hold '+' and '-' together to open the FullscreenUI pause menu
	constexpr u64 MENU_COMBO = HidNpadButton_Plus | HidNpadButton_Minus;

	std::unique_ptr<INISettingsInterface> s_settings_interface;

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

	// Map by physical position rather than meaning
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

	float ApplyDeadzone(s32 raw)
	{
		const float v = std::clamp(static_cast<float>(raw) / static_cast<float>(JOYSTICK_MAX), -1.0f, 1.0f);
		const float mag = std::fabs(v);
		if (mag < STICK_DEADZONE)
			return 0.0f;
		const float scaled = (mag - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
		return (v < 0.0f) ? -scaled : scaled;
	}

	void ApplyStick(const HidAnalogStickState& s, PadDualshock2::Inputs left, PadDualshock2::Inputs right,
		PadDualshock2::Inputs up, PadDualshock2::Inputs down)
	{
		const float x = ApplyDeadzone(s.x);
		const float y = ApplyDeadzone(s.y);
		Pad::SetControllerState(0, static_cast<u32>(right), x > 0.0f ? x : 0.0f);
		Pad::SetControllerState(0, static_cast<u32>(left), x < 0.0f ? -x : 0.0f);
		Pad::SetControllerState(0, static_cast<u32>(up), y > 0.0f ? y : 0.0f);
		Pad::SetControllerState(0, static_cast<u32>(down), y < 0.0f ? -y : 0.0f);
	}

	void FeedGamePad(PadState& pad, u64 held)
	{
		for (const ButtonMap& m : BUTTON_MAP)
			Pad::SetControllerState(0, static_cast<u32>(m.ps2), (held & m.nx) ? 1.0f : 0.0f);

		ApplyStick(padGetStickPos(&pad, 0), PadDualshock2::Inputs::PAD_L_LEFT, PadDualshock2::Inputs::PAD_L_RIGHT,
			PadDualshock2::Inputs::PAD_L_UP, PadDualshock2::Inputs::PAD_L_DOWN);
		ApplyStick(padGetStickPos(&pad, 1), PadDualshock2::Inputs::PAD_R_LEFT, PadDualshock2::Inputs::PAD_R_RIGHT,
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

	void SetupSettings()
	{
		const std::string ini_path = Path::Combine(ARMSX2_ROOT, "armsx2.ini");
		s_settings_interface = std::make_unique<INISettingsInterface>(ini_path);
		Host::Internal::SetBaseSettingsLayer(s_settings_interface.get());
		s_settings_interface->Load();

		if (s_settings_interface->IsEmpty())
		{
			INFO_LOG("No settings found; writing default settings to {}", ini_path);
			VMManager::SetDefaultSettings(*s_settings_interface, true, true, true, true, true);

			s_settings_interface->SetStringValue("Filenames", "Game", "");
			s_settings_interface->SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::DK3D));
			s_settings_interface->SetStringValue("EmuCore/GS", "AspectRatio", "4:3");
			s_settings_interface->SetIntValue("EmuCore/GS", "deinterlace_mode", static_cast<int>(GSInterlaceMode::Off));
			s_settings_interface->SetStringValue("SPU2/Output", "Backend", "Horizon");
			s_settings_interface->SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
			s_settings_interface->SetIntValue("EmuCore/GS", "VsyncEnable", 0);
			s_settings_interface->SetBoolValue("UI", "EnableFullscreenUI", true);
			s_settings_interface->AddToStringList("GameList", "RecursivePaths", GAMES_DIR);
			s_settings_interface->SetBoolValue("Achievements", "Enabled", false);
			s_settings_interface->SetBoolValue("InputSources", "SDL", false);
			s_settings_interface->SetBoolValue("Logging", "EnableSystemConsole", true);
			s_settings_interface->SetBoolValue("Logging", "EnableVerbose", false);
			s_settings_interface->Save();
		}

		VMManager::Internal::LoadStartupSettings();
		EmuFolders::EnsureFoldersExist();
	}

	// Boot a disc image directly when passed as argv[1]
	void BootImage(std::string path)
	{
		VMBootParameters params;
		params.filename = std::move(path);

		INFO_LOG("Booting image: {}", params.filename);
		if (VMManager::Initialize(std::move(params)))
			VMManager::SetState(VMState::Running);
		else
			ERROR_LOG("VMManager::Initialize() failed for the launch image");
	}
} // namespace

void Host::CommitBaseSettingChanges()
{
	auto lock = Host::GetSettingsLock();
	if (s_settings_interface)
		s_settings_interface->Save();
}

int main(int argc, char** argv)
{
	const bool have_socket = R_SUCCEEDED(socketInitializeDefault());
	if (have_socket)
		nxlinkStdio();

	mkdir("sdmc:/switch", 0777);
	mkdir(ARMSX2_ROOT, 0777);
	mkdir(GAMES_DIR, 0777);

	Log::SetTimestampsEnabled(true);
	Log::SetConsoleOutputLevel(LOGLEVEL_INFO);
	if (!Log::SetFileOutputLevel(LOGLEVEL_DEV, LOG_PATH))
		std::fprintf(stderr, "WARNING: could not open SD log file %s\n", LOG_PATH);

	INFO_LOG("================ ARMSX2-NX ================");
	INFO_LOG("Logging to {}", LOG_PATH);

	// Mount the romfs so Deko3D can load its shaders
	const bool have_romfs = R_SUCCEEDED(romfsInit());
	if (!have_romfs)
		ERROR_LOG("romfsInit() failed. Deko3d shaders will be unavailable. Things will be broken");

	EmuFolders::AppRoot = ARMSX2_ROOT;
	EmuFolders::DataRoot = ARMSX2_ROOT;
	EmuFolders::SetResourcesDirectory();
	SetupSettings();

	INFO_LOG("BIOS directory: {}", EmuFolders::Bios);
	const std::string bios = s_settings_interface->GetStringValue("Filenames", "BIOS", "");
	if (bios.empty())
		ERROR_LOG("No BIOS configured. Put a dumped BIOS in {} and set [Filenames] BIOS=<file> "
				  "in armsx2.ini", EmuFolders::Bios);
	else
		INFO_LOG("Configured BIOS: {}", bios);

	// Point ImGui at its fonts
	ImGuiManager::SetFontPathAndRange(
		Path::Combine(EmuFolders::Resources, "fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf"), {});

	if (!VMManager::Internal::CPUThreadInitialize())
	{
		ERROR_LOG("CPUThreadInitialize() failed. Aborting...");
		VMManager::Internal::CPUThreadShutdown();
		if (have_romfs)
			romfsExit();
		if (have_socket)
			socketExit();
		return 1;
	}

	VMManager::ApplySettings();

	HorizonHost::SetCPUThread();

	if (!MTGS::WaitForOpen())
	{
		ERROR_LOG("Failed to open GS; aborting.");
		VMManager::Internal::CPUThreadShutdown();
		if (have_romfs)
			romfsExit();
		if (have_socket)
			socketExit();
		return 1;
	}

	const bool enable_fsui = s_settings_interface->GetBoolValue("UI", "EnableFullscreenUI", true);
	if (enable_fsui)
	{
		MTGS::RunOnGSThread(&ImGuiManager::InitializeFullscreenUI);
		Host::RefreshGameListAsync(false);
	}

	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	PadState pad;
	padInitializeDefault(&pad);

	std::string boot_image;
	if (argc > 1 && argv[1] && argv[1][0])
		boot_image = argv[1];

	if (!boot_image.empty() && FileSystem::FileExists(boot_image.c_str()))
		BootImage(std::move(boot_image));
	else if (!boot_image.empty())
		ERROR_LOG("Launch image not found: {}. Returing to game selector.", boot_image);
	else if (!enable_fsui)
		INFO_LOG("FullscreenUI disabled and no launch image. Please provide a target to display.");

	std::atomic_bool stop_loop{false};
	std::thread input_thread([&]() {
		u64 prev_held = 0;
		bool prev_menu_combo = false;
		while (!stop_loop.load(std::memory_order_relaxed))
		{
			padUpdate(&pad);
			const u64 held = padGetButtons(&pad);
			const u64 changed = held ^ prev_held;
			prev_held = held;

			const bool fsui_active = FullscreenUI::HasActiveWindow();
			if (fsui_active)
				FeedNav(held, changed);
			else
				FeedGamePad(pad, held);

			const bool menu_combo = (held & MENU_COMBO) == MENU_COMBO;
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
	});

	while (!HorizonHost::IsExitRequested())
	{
		Host::PumpMessagesOnCPUThread();

		switch (VMManager::GetState())
		{
			case VMState::Running:
				VMManager::Execute(); // returns when paused or stopping
				break;

			case VMState::Stopping:
				VMManager::Shutdown(false);
				break;

			default:
				// Idle while the GS thread presents FullscreenUI
				svcSleepThread(IDLE_POLL_NS);
				break;
		}
	}

	stop_loop.store(true, std::memory_order_relaxed);
	input_thread.join();

	Host::CancelGameListRefresh();

	if (VMManager::GetState() != VMState::Shutdown)
		VMManager::Shutdown(false);

	MTGS::WaitForClose();
	VMManager::Internal::CPUThreadShutdown();
	INFO_LOG("================ Exiting ================");

	if (have_romfs)
		romfsExit();
	if (have_socket)
		socketExit();
	return 0;
}
