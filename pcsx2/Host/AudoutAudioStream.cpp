// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

// Audout audio backend

#include "Host/AudioStream.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/Horizon/Horizon.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace
{
	class AudoutAudioStream final : public AudioStream
	{
	public:
		AudoutAudioStream(u32 sample_rate, const AudioStreamParameters& parameters);
		~AudoutAudioStream() override;

		bool Initialize(bool stretch_enabled, Error* error);

	private:
		static constexpr u32 NUM_CHANNELS = 2;
		// Frames per submitted buffer. 1024 frames * 2ch * 2 bytes (s16) = 0x1000.
		static constexpr u32 BUFFER_FRAMES = 1024;
		static constexpr u32 BUFFER_SAMPLES = BUFFER_FRAMES * NUM_CHANNELS;
		static constexpr u64 BUFFER_BYTES = BUFFER_SAMPLES * sizeof(s16);
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

		std::array<SampleType, BUFFER_SAMPLES> m_float_scratch = {};
	};
} // namespace

AudoutAudioStream::AudoutAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
	: AudioStream(sample_rate, parameters)
{
}

AudoutAudioStream::~AudoutAudioStream()
{
	Destroy();
}

void AudoutAudioStream::FillBuffer(AudioOutBuffer& buf)
{
	ReadFrames(m_float_scratch.data(), BUFFER_FRAMES);

	s16* const out = static_cast<s16*>(buf.buffer);
	for (u32 i = 0; i < BUFFER_SAMPLES; i++)
	{
		const float f = std::clamp(m_float_scratch[i], -1.0f, 1.0f);
		out[i] = static_cast<s16>(std::lrintf(f * 32767.0f));
	}

	buf.data_size = BUFFER_BYTES;
	buf.data_offset = 0;
}

void AudoutAudioStream::ThreadEntry()
{
	for (AudioOutBuffer& buf : m_buffers)
	{
		FillBuffer(buf);
		audoutAppendAudioOutBuffer(&buf);
	}

	while (!m_quit.load(std::memory_order_relaxed))
	{
		AudioOutBuffer* released = nullptr;
		u32 released_count = 0;
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

bool AudoutAudioStream::Initialize(bool stretch_enabled, Error* error)
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
	m_thread = std::thread(&AudoutAudioStream::ThreadEntry, this);
	return true;
}

void AudoutAudioStream::Destroy()
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

std::unique_ptr<AudioStream> AudioStream::CreateAudoutAudioStream(u32 sample_rate,
	const AudioStreamParameters& parameters, bool stretch_enabled, Error* error)
{
	AudioStreamParameters params = parameters;
	params.expansion_mode = AudioExpansionMode::Disabled;

	std::unique_ptr<AudoutAudioStream> stream = std::make_unique<AudoutAudioStream>(sample_rate, params);
	if (!stream->Initialize(stretch_enabled, error))
		stream.reset();

	return stream;
}
