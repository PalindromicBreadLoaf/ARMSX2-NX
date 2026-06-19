// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "pcsx2/Config.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
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

namespace
{
	constexpr const char* ARMSX2_ROOT = "sdmc:/switch/armsx2";
	constexpr const char* LOG_PATH = "sdmc:/switch/armsx2/armsx2.log";

	constexpr u64 INPUT_POLL_NS = 16'000'000ULL;

	constexpr float STICK_DEADZONE = 0.15f;

	// Hold '+' and '-' to return to launcher
	constexpr u64 QUIT_COMBO = HidNpadButton_Plus | HidNpadButton_Minus;

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

	u64 PollAndApplyInput(PadState& pad)
	{
		padUpdate(&pad);
		const u64 held = padGetButtons(&pad);

		for (const ButtonMap& m : BUTTON_MAP)
			Pad::SetControllerState(0, static_cast<u32>(m.ps2), (held & m.nx) ? 1.0f : 0.0f);

		ApplyStick(padGetStickPos(&pad, 0), PadDualshock2::Inputs::PAD_L_LEFT, PadDualshock2::Inputs::PAD_L_RIGHT,
			PadDualshock2::Inputs::PAD_L_UP, PadDualshock2::Inputs::PAD_L_DOWN);
		ApplyStick(padGetStickPos(&pad, 1), PadDualshock2::Inputs::PAD_R_LEFT, PadDualshock2::Inputs::PAD_R_RIGHT,
			PadDualshock2::Inputs::PAD_R_UP, PadDualshock2::Inputs::PAD_R_DOWN);

		return held;
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
			s_settings_interface->SetIntValue("EmuCore/GS", "deinterlace_mode", static_cast<int>(GSInterlaceMode::Off));
			s_settings_interface->SetStringValue("SPU2/Output", "Backend", "Horizon");
			s_settings_interface->SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
			s_settings_interface->SetIntValue("EmuCore/GS", "VsyncEnable", 0);
			s_settings_interface->SetBoolValue("UI", "EnableFullscreenUI", false);
			s_settings_interface->SetBoolValue("Achievements", "Enabled", false);
			s_settings_interface->SetBoolValue("InputSources", "SDL", false);
			s_settings_interface->SetBoolValue("Logging", "EnableSystemConsole", true);
			s_settings_interface->SetBoolValue("Logging", "EnableVerbose", false);
			s_settings_interface->Save();
		}

		VMManager::Internal::LoadStartupSettings();
		EmuFolders::EnsureFoldersExist();
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

	if (!VMManager::Internal::CPUThreadInitialize())
	{
		ERROR_LOG("CPUThreadInitialize() failed; aborting.");
		VMManager::Internal::CPUThreadShutdown();
		if (have_romfs)
			romfsExit();
		if (have_socket)
			socketExit();
		return 1;
	}

	VMManager::ApplySettings();

	VMBootParameters boot_params;
	std::string boot_image;
	if (argc > 1 && argv[1] && argv[1][0])
		boot_image = argv[1];
	else
		boot_image = s_settings_interface->GetStringValue("Filenames", "Game", "");

	if (!boot_image.empty() && FileSystem::FileExists(boot_image.c_str()))
	{
		boot_params.filename = std::move(boot_image);
		INFO_LOG("Booting image: {}", boot_params.filename);
	}
	else
	{
		if (!boot_image.empty())
			ERROR_LOG("Game image not found: {}. Booting PS2 BIOS.", boot_image);
		else
			INFO_LOG("Booting PS2 BIOS.");
		boot_params.fast_boot = false;
	}

	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	PadState pad;
	padInitializeDefault(&pad);

	if (VMManager::Initialize(boot_params))
	{
		VMManager::SetState(VMState::Running);

		std::atomic_bool stop_loop{false};
		std::thread input_thread([&]() {
			while (!stop_loop.load(std::memory_order_relaxed))
			{
				const u64 held = PollAndApplyInput(pad);
				if ((held & QUIT_COMBO) == QUIT_COMBO)
				{
					INFO_LOG("'+'&'-' pressed; stopping.");
					break;
				}
				svcSleepThread(INPUT_POLL_NS);
			}
			VMManager::SetState(VMState::Stopping);
		});

		while (true)
		{
			const VMState state = VMManager::GetState();
			if (state == VMState::Stopping || state == VMState::Shutdown)
				break;
			else if (state == VMState::Running)
				VMManager::Execute();
			else
				svcSleepThread(50'000'000ULL);
		}

		stop_loop.store(true, std::memory_order_relaxed);
		input_thread.join();

		VMManager::Shutdown(false);
	}
	else
	{
		ERROR_LOG("VMManager::Initialize() failed. Check your BIOS path/dump.");
	}

	VMManager::Internal::CPUThreadShutdown();
	INFO_LOG("================ Exiting ================");

	if (have_romfs)
		romfsExit();
	if (have_socket)
		socketExit();
	return 0;
}
