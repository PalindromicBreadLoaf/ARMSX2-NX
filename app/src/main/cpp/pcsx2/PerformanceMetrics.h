// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <array>
#include "common/Threading.h"

namespace PerformanceMetrics
{
	enum class InternalFPSMethod
	{
		None,
		GSPrivilegedRegister,
		DISPFBBlit
	};

	enum class Limiter
	{
		Unknown,
		EE,
		VU,
		GSThread,
		GPU,
		FrameLimited
	};

	static constexpr u32 NUM_FRAME_TIME_SAMPLES = 150;
	using FrameTimeHistory = std::array<float, NUM_FRAME_TIME_SAMPLES>;

	void Clear();
	void Reset();
	void Update(bool gs_register_write, bool fb_blit, bool is_skipping_present);
	void OnGPUPresent(float gpu_time);

	/// Sets the EE thread for CPU usage calculations.
	void SetCPUThread(Threading::ThreadHandle thread);

	/// Sets timers for GS software threads.
	void SetGSSWThreadCount(u32 count);
	void SetGSSWThread(u32 index, Threading::ThreadHandle thread);

	u64 GetFrameNumber();

	InternalFPSMethod GetInternalFPSMethod();
	bool IsInternalFPSValid();

	float GetFPS();
	float GetInternalFPS();
	float GetSpeed();
	float GetAverageFrameTime();
	float GetMinimumFrameTime();
	float GetMaximumFrameTime();

	double GetCPUThreadUsage();
	double GetCPUThreadAverageTime();
	float GetGSThreadUsage();
	float GetGSThreadAverageTime();
	float GetVUThreadUsage();
	float GetVUThreadAverageTime();
	float GetCaptureThreadUsage();
	float GetCaptureThreadAverageTime();

	u32 GetGSSWThreadCount();
	double GetGSSWThreadUsage(u32 index);
	double GetGSSWThreadAverageTime(u32 index);

	float GetGPUUsage();
	float GetGPUAverageTime();

	/// Marks the calling thread as the EE thread
	void MarkEEThread();

	/// Accumulates time the EE thread spent waiting on VU/GS threads.
	void AccumulateEEStallVU(u64 ns);
	void AccumulateEEStallGS(u64 ns);
	void AccumulateEEStallVsync(u64 ns);

	/// Same as above but for GS
	void AccumulateGSAcquireWait(u64 ns);
	void AccumulateGSGpuWait(u64 ns);
	/// Time the GS thread spent idle waiting for the EE thread
	void AccumulateGSWorkWait(u64 ns);

	/// Average milliseconds per frame the EE thread spent blocked on VU / GS threads.
	float GetEEStallVUTime();
	float GetEEStallGSTime();
	float GetEEStallVsyncTime();

	/// Average milliseconds per frame the GS thread spent blocked on image acquisition / GPU fence / idle.
	float GetGSAcquireWaitTime();
	float GetGSGpuWaitTime();
	float GetGSWorkWaitTime();

	/// The synthesized per-frame limiter verdict and the busy fraction of each pipeline stage.
	Limiter GetLimiter();
	const char* GetLimiterName();
	float GetEEBusyPercent();
	float GetGSBusyPercent();
	float GetGPUBusyPercent();

	const FrameTimeHistory& GetFrameTimeHistory();
	u32 GetFrameTimeHistoryPos();
} // namespace PerformanceMetrics
