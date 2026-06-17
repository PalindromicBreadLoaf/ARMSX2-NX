// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

// This is a temporary bring-up just to test other things as development goes on
// Currently hard set to boot the BIOS
#include "common/Console.h"
#include "common/Path.h"

#include "pcsx2/Config.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/VMManager.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>

#include "common/Horizon/Horizon.h"

namespace
{
	constexpr const char* HARNESS_ROOT = "sdmc:/switch/armsx2";
	constexpr const char* HARNESS_LOG = "sdmc:/switch/armsx2/armsx2.log";

	// Run for how long?
	constexpr int RUN_SECONDS = 60;

	std::unique_ptr<INISettingsInterface> s_settings_interface;

	void ProbeJitCapability()
	{
		Jit jit;
		const Result rc = jitCreate(&jit, 0x1000);
		if (R_SUCCEEDED(rc))
		{
			INFO_LOG("JIT succeeded");
			jitClose(&jit);
		}
		else
		{
			ERROR_LOG("JIT failed", rc);
		}
	}

	void SetupSettings()
	{
		const std::string ini_path = Path::Combine(HARNESS_ROOT, "armsx2.ini");
		s_settings_interface = std::make_unique<INISettingsInterface>(ini_path);
		Host::Internal::SetBaseSettingsLayer(s_settings_interface.get());
		s_settings_interface->Load();

		if (s_settings_interface->IsEmpty())
		{
			INFO_LOG("No settings found; writing headless defaults to {}", ini_path);
			VMManager::SetDefaultSettings(*s_settings_interface, true, true, true, true, true);
		}

		// Null GS renderer
		// Null audio backend + sink
		// DEFAULT_BACKEND is Oboe, which isn't built, so null that.
		// no frame limiter: let the BIOS run as fast as the CPU allows (not very I assume)
		// no fullscreen UI
		// IOP/VU recompilers are left enabled
		s_settings_interface->SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::Null));
		s_settings_interface->SetStringValue("SPU2/Output", "OutputModule", "nullout");
		s_settings_interface->SetStringValue("SPU2/Output", "Backend", "Null");
		s_settings_interface->SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
		s_settings_interface->SetIntValue("EmuCore/GS", "VsyncEnable", 0);
		s_settings_interface->SetBoolValue("UI", "EnableFullscreenUI", false);
		s_settings_interface->SetBoolValue("Achievements", "Enabled", false);
		s_settings_interface->SetBoolValue("InputSources", "SDL", false);
		s_settings_interface->SetBoolValue("Logging", "EnableSystemConsole", true);
		s_settings_interface->SetBoolValue("Logging", "EnableVerbose", false);
		s_settings_interface->Save();

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
	// Best-effort networked stdout for `nxlink -s`
	const bool have_socket = R_SUCCEEDED(socketInitializeDefault());
	if (have_socket)
		nxlinkStdio();

	// The SD log directory must exist before we open the log file.
	mkdir("sdmc:/switch", 0777);
	mkdir(HARNESS_ROOT, 0777);

	Log::SetTimestampsEnabled(true);
	Log::SetConsoleOutputLevel(LOGLEVEL_INFO); // -> stdout / nxlink
	if (!Log::SetFileOutputLevel(LOGLEVEL_DEV, HARNESS_LOG))
		std::fprintf(stderr, "WARNING: could not open SD log file %s\n", HARNESS_LOG);

	INFO_LOG("================ ARMSX2 headless ================");
	INFO_LOG("Logging to {}", HARNESS_LOG);

	ProbeJitCapability();

	EmuFolders::AppRoot = HARNESS_ROOT;
	EmuFolders::DataRoot = HARNESS_ROOT;
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
		if (have_socket)
			socketExit();
		return 1;
	}

	VMManager::ApplySettings();

	VMBootParameters boot_params;
	if (argc > 1 && argv[1] && argv[1][0])
	{
		boot_params.filename = argv[1]; // ISO/ELF
		INFO_LOG("Booting image: {}", boot_params.filename);
	}
	else
	{
		boot_params.fast_boot = false; // boot BIOS
		INFO_LOG("No image given; booting the PS2 BIOS.");
	}

	// Pad, used only so the user can quit early with '+'.
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	PadState pad;
	padInitializeDefault(&pad);

	if (VMManager::Initialize(boot_params))
	{
		VMManager::SetState(VMState::Running);

		// Stop after RUN_SECONDS or immediately if '+' is pressed
		std::atomic_bool stop_loop{false};
		std::thread watchdog([&]() {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(RUN_SECONDS);
			while (!stop_loop.load(std::memory_order_relaxed))
			{
				padUpdate(&pad);
				if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
				{
					INFO_LOG("'+' pressed; stopping.");
					break;
				}
				if (std::chrono::steady_clock::now() >= deadline)
				{
					INFO_LOG("Run time limit ({}s) reached. Stopping.", RUN_SECONDS);
					break;
				}
				svcSleepThread(50'000'000ULL); // 50 ms
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
		watchdog.join();

		VMManager::Shutdown(false);
	}
	else
	{
		ERROR_LOG("VMManager::Initialize() failed. Check the BIOS path/dump above.");
	}

	VMManager::Internal::CPUThreadShutdown();
	INFO_LOG("================ Exiting ================");

	if (have_socket)
		socketExit();
	return 0;
}
