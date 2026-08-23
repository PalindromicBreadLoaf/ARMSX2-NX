// SPDX-FileCopyrightText: 2026 ARMSX2 Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <string>
#include <vector>

namespace HorizonUsbStorage
{
struct Volume
{
	std::string root;
	std::string label;
	std::string filesystem;
};

bool Initialize();
void Shutdown();

bool IsAvailable();
const std::string& GetError();
std::vector<Volume> GetVolumes();

// Returns true once per attach/removal event.
bool ConsumeChange();
} // namespace HorizonUsbStorage
