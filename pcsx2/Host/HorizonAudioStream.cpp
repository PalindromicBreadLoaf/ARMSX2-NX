// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

// libnx audout backend for the Nintendo Switch (Horizon) port.
//
// audout is a push model with no data callback: buffers are submitted with
// audoutAppendAudioOutBuffer() and recycled once the hardware has consumed them
// (audoutWaitPlayFinish()).
//
// audout is fixed at 48 kHz / stereo / s16

#include "Host/AudioStream.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/Horizon/Horizon.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace
{
	class HorizonAudioStream final : public AudioStream
	{
	public:
		HorizonAudioStream(u32 sample_rate, const AudioStreamParameters& parameters);
		~HorizonAudioStream() override;

		bool Initialize(bool stretch_enabled, Error* error);

	private:
		static constexpr u32 NUM_CHANNELS = 2;
		// Frames per submitted buffer. 1024 frames * 2ch * 2 bytes
		static constexpr u32 BUFFER_FRAMES = 1024;
		static constexpr u64 BUFFER_BYTES = BUFFER_FRAMES * NUM_CHANNELS * sizeof(SampleType);
		static_assert((BUFFER_BYTES % 0x1000) == 0, "audout buffers must be a multiple of 0x1000");
		static constexpr u32 NUM_BUFFERS = 4;

		void ThreadEntry();
		void FillBuffer(AudioOutBuffer& buf);
		void Destroy();

		std::thread m_thread;
		std::atomic_bool m_quit{false};
		bool m_audout_open = false;

		std::array<AudioOutBuffer, NUM_BUFFERS> m_buffers = {};
		std::array<void*, NUM_BUFFERS> m_buffer_mem = {};
	};
} // namespace

HorizonAudioStream::HorizonAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
	: AudioStream(sample_rate, parameters)
{
}

HorizonAudioStream::~HorizonAudioStream()
{
	Destroy();
}

void HorizonAudioStream::FillBuffer(AudioOutBuffer& buf)
{
	ReadFrames(static_cast<SampleType*>(buf.buffer), BUFFER_FRAMES);
	buf.data_size = BUFFER_BYTES;
	buf.data_offset = 0;
}

void HorizonAudioStream::ThreadEntry()
{
	// Prime and queue all buffers before first completion.
	for (AudioOutBuffer& buf : m_buffers)
	{
		FillBuffer(buf);
		audoutAppendAudioOutBuffer(&buf);
	}

	while (!m_quit.load(std::memory_order_relaxed))
	{
		AudioOutBuffer* released = nullptr;
		u32 released_count = 0;
		// 100ms timeout so the quit flag stays responsive on shutdown.
		const Result rc = audoutWaitPlayFinish(&released, &released_count, 100'000'000ULL);
		if (R_FAILED(rc) || released_count == 0)
			continue;

		for (AudioOutBuffer* buf = released; buf != nullptr;)
		{
			AudioOutBuffer* const next = buf->next;
			FillBuffer(*buf);
			audoutAppendAudioOutBuffer(buf);
			buf = next;
		}
	}
}

bool HorizonAudioStream::Initialize(bool stretch_enabled, Error* error)
{
	BaseInitialize(&StereoSampleReaderImpl, stretch_enabled);

	Result rc = audoutInitialize();
	if (R_FAILED(rc))
	{
		Error::SetStringFmt(error, "audoutInitialize() failed: 0x{:08x}", rc);
		return false;
	}
	m_audout_open = true;

	rc = audoutStartAudioOut();
	if (R_FAILED(rc))
	{
		Error::SetStringFmt(error, "audoutStartAudioOut() failed: 0x{:08x}", rc);
		return false;
	}

	const u32 device_rate = audoutGetSampleRate();
	if (m_sample_rate != device_rate)
	{
		Console.Warning("stream rate %u != device rate %u. audout does not support resampling, "
						"pitch may be off.", m_sample_rate, device_rate);
	}

	for (u32 i = 0; i < NUM_BUFFERS; i++)
	{
		void* const mem = std::aligned_alloc(0x1000, BUFFER_BYTES);
		if (!mem)
		{
			Error::SetStringView(error, "Failed to allocate audout sample buffer.");
			return false;
		}

		std::memset(mem, 0, BUFFER_BYTES);
		m_buffer_mem[i] = mem;
		m_buffers[i].next = nullptr;
		m_buffers[i].buffer = mem;
		m_buffers[i].buffer_size = BUFFER_BYTES;
		m_buffers[i].data_size = BUFFER_BYTES;
		m_buffers[i].data_offset = 0;
	}

	Console.WriteLn("audout started: %u Hz, %u channels, %u x %u-frame buffers.",
		device_rate, audoutGetChannelCount(), NUM_BUFFERS, BUFFER_FRAMES);

	m_quit.store(false, std::memory_order_relaxed);
	m_thread = std::thread(&HorizonAudioStream::ThreadEntry, this);
	return true;
}

void HorizonAudioStream::Destroy()
{
	if (m_thread.joinable())
	{
		m_quit.store(true, std::memory_order_relaxed);
		m_thread.join();
	}

	if (m_audout_open)
	{
		audoutStopAudioOut();
		audoutExit();
		m_audout_open = false;
	}

	for (void*& mem : m_buffer_mem)
	{
		std::free(mem);
		mem = nullptr;
	}
}

std::unique_ptr<AudioStream> AudioStream::CreateHorizonAudioStream(u32 sample_rate,
	const AudioStreamParameters& parameters, bool stretch_enabled, Error* error)
{
	AudioStreamParameters params = parameters;
	params.expansion_mode = AudioExpansionMode::Disabled;

	std::unique_ptr<HorizonAudioStream> stream = std::make_unique<HorizonAudioStream>(sample_rate, params);
	if (!stream->Initialize(stretch_enabled, error))
		stream.reset();

	return stream;
}
