// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/DK3D/DKShaderCompiler.h"

#ifdef __SWITCH__

#include "GS/GSXXH.h"

#include "Config.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/Timer.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#ifdef DK3D_RUNTIME_SHADERS
#include "GS/Renderers/DK3D/uam/UamBridge.h"
#endif

bool DKTfxSelector::operator==(const DKTfxSelector& rhs) const
{
	return std::memcmp(this, &rhs, sizeof(DKTfxSelector)) == 0;
}

namespace
{
	// Bump when the preamble format or anything else that changes generated code changes shape.
	constexpr u32 CACHE_FORMAT_VERSION = 1;
	constexpr u32 CACHE_MAGIC = 0x4B534B44; // 'DKSK'
	constexpr u32 MAX_BLOB_SIZE = 1 * 1024 * 1024;
	constexpr u64 MAX_CACHE_BYTES = 32 * 1024 * 1024;

	struct CacheFileHeader
	{
		u32 magic;
		u32 format_version;
		u64 source_hash;
		u32 selector_size;
		u32 reserved;
	};

	struct CompileRequest
	{
		DKTfxSelector sel;
		u64 hash;
	};

	struct CompileResult
	{
		DKTfxSelector sel;
		u64 hash;
		std::vector<u8> blob;
	};

	struct DiskEntry
	{
		DKTfxSelector sel;
		std::vector<u8> blob;
	};

	struct CompilerState
	{
		bool open = false;

		// tfx_fsh.glsl
		std::string version_line;
		std::string body;
		u64 source_hash = 0;

		std::thread worker;
		std::mutex lock;
		std::condition_variable cv;
		std::deque<CompileRequest> queue;
		std::deque<CompileResult> results;
		std::unordered_map<u64, DiskEntry> disk;
		u32 in_flight = 0;
		bool shutdown = false;

		// Append-only handle onto the cache file.
		std::mutex file_lock;
		std::FILE* file = nullptr;
		u64 file_bytes = 0;

		u32 compiled_count = 0;
		u32 failed_count = 0;
		double total_compile_ms = 0.0;
	};

	CompilerState s_state;

	std::string GetCacheFileName()
	{
		return Path::Combine(EmuFolders::Cache, "dk3d_tfx_shaders.bin");
	}

	bool LoadShaderSource()
	{
		std::FILE* fp = FileSystem::OpenCFile("romfs:/shaders/tfx_fsh.glsl", "rb");
		if (!fp)
		{
			Console.Warning("DK3D: tfx_fsh.glsl is not in the romfs.");
			return false;
		}

		std::string source;
		char buf[4096];
		size_t read;
		while ((read = std::fread(buf, 1, sizeof(buf), fp)) > 0)
			source.append(buf, read);
		std::fclose(fp);

		if (source.empty())
			return false;

		// uam wants #version first.
		const size_t version_pos = source.find("#version");
		if (version_pos == std::string::npos)
		{
			Console.Warning("DK3D: tfx_fsh.glsl has no #version directive.");
			return false;
		}
		size_t version_end = source.find('\n', version_pos);
		version_end = (version_end == std::string::npos) ? source.size() : version_end + 1;

		s_state.version_line = source.substr(0, version_end);
		s_state.body = source.substr(version_end);
		s_state.source_hash = GSXXH3_64bits(source.data(), source.size());
		return true;
	}

	// Reads whatever the last run left behind. Anything built by a different source is
	// trashed on read.
	bool LoadDiskCache(const std::string& filename)
	{
		std::FILE* fp = FileSystem::OpenCFile(filename.c_str(), "rb");
		if (!fp)
			return false;

		CacheFileHeader header;
		bool ok = std::fread(&header, sizeof(header), 1, fp) == 1 && header.magic == CACHE_MAGIC &&
				  header.format_version == CACHE_FORMAT_VERSION && header.source_hash == s_state.source_hash &&
				  header.selector_size == sizeof(DKTfxSelector);

		u32 loaded = 0;
		bool clean_eof = false;
		while (ok)
		{
			DKTfxSelector sel;
			u32 blob_size;
			const size_t selector_bytes = std::fread(&sel, 1, sizeof(sel), fp);
			if (selector_bytes == 0 && std::feof(fp))
			{
				clean_eof = true;
				break;
			}
			if (selector_bytes != sizeof(sel) || std::fread(&blob_size, sizeof(blob_size), 1, fp) != 1)
				break;

			if (blob_size == 0 || blob_size > MAX_BLOB_SIZE)
			{
				ok = false;
				break;
			}

			std::vector<u8> blob(blob_size);
			if (std::fread(blob.data(), 1, blob_size, fp) != blob_size)
			{
				clean_eof = false;
				break;
			}

			const u64 hash = DKShaderCompiler::HashSelector(sel);
			s_state.disk.emplace(hash, DiskEntry{sel, std::move(blob)});
			loaded++;
		}
		std::fclose(fp);

		if (!ok)
		{
			s_state.disk.clear();
			FileSystem::DeleteFilePath(filename.c_str());
			return false;
		}

		if (loaded > 0)
			Console.WriteLn("DK3D: recovered %u tfx shaders from the shader cache.", loaded);

		return !clean_eof;
	}

	void OpenCacheForAppend(const std::string& filename, bool rewrite)
	{
		const bool exists = !rewrite && FileSystem::FileExists(filename.c_str());
		s_state.file = FileSystem::OpenCFile(filename.c_str(), exists ? "r+b" : "w+b");
		if (!s_state.file)
		{
			Console.Warning("DK3D: could not open %s for writing. Compiled shaders will not persist.",
				filename.c_str());
			return;
		}

		std::setvbuf(s_state.file, nullptr, _IOFBF, 1 * 1024 * 1024);

		if (exists)
		{
			FileSystem::FSeek64(s_state.file, 0, SEEK_END);
			s_state.file_bytes = static_cast<u64>(FileSystem::FTell64(s_state.file));
		}
		else
		{
			const CacheFileHeader header = {
				CACHE_MAGIC, CACHE_FORMAT_VERSION, s_state.source_hash, sizeof(DKTfxSelector), 0};
			bool write_ok = std::fwrite(&header, sizeof(header), 1, s_state.file) == 1;
			s_state.file_bytes = sizeof(header);

			// Put back everything that survived a damaged tail.
			for (const auto& [hash, entry] : s_state.disk)
			{
				const u32 blob_size = static_cast<u32>(entry.blob.size());
				write_ok = write_ok && std::fwrite(&entry.sel, sizeof(entry.sel), 1, s_state.file) == 1 &&
						   std::fwrite(&blob_size, sizeof(blob_size), 1, s_state.file) == 1 &&
						   std::fwrite(entry.blob.data(), 1, entry.blob.size(), s_state.file) == entry.blob.size();
				if (!write_ok)
					break;
				s_state.file_bytes += sizeof(entry.sel) + sizeof(blob_size) + entry.blob.size();
			}

			if (!write_ok)
			{
				Console.Warning("DK3D: shader cache rewrite failed. Compiled shaders will not persist.");
				std::fclose(s_state.file);
				s_state.file = nullptr;
				s_state.file_bytes = 0;
			}
		}
	}

	void AppendToDiskCache(const DKTfxSelector& sel, const std::vector<u8>& blob)
	{
		std::lock_guard<std::mutex> guard(s_state.file_lock);
		const u64 record_size = sizeof(sel) + sizeof(u32) + blob.size();
		if (!s_state.file || s_state.file_bytes >= MAX_CACHE_BYTES ||
			record_size > MAX_CACHE_BYTES - s_state.file_bytes)
			return;

		const u32 blob_size = static_cast<u32>(blob.size());
		if (std::fwrite(&sel, sizeof(sel), 1, s_state.file) != 1 ||
			std::fwrite(&blob_size, sizeof(blob_size), 1, s_state.file) != 1 ||
			std::fwrite(blob.data(), 1, blob.size(), s_state.file) != blob.size())
		{
			Console.Warning("DK3D: shader cache write failed. Compiled shaders will not persist.");
			std::fclose(s_state.file);
			s_state.file = nullptr;
			return;
		}
		s_state.file_bytes += record_size;
	}

	// Turns a selector into the `const uint sel_*` block that lets mesa fold every branch away.
	std::string BuildSpecializedSource(const DKTfxSelector& sel)
	{
		std::string source;
		source.reserve(s_state.version_line.size() + s_state.body.size() + 2048);
		source.append(s_state.version_line);
		source.append("#define TFX_SPECIALIZED 1\n");

		char line[128];
#define DK_TFX_EMIT_FIELD(name) \
	std::snprintf(line, sizeof(line), "const uint sel_" #name " = %uu;\n", sel.name); \
	source.append(line);
		DK_TFX_SELECTOR_FIELDS(DK_TFX_EMIT_FIELD)
#undef DK_TFX_EMIT_FIELD

		// Both operands are constants by now, so this folds like the baked variants do.
		source.append("#define WRAP_IS_SIMPLE (sel_wms < 2u && sel_wmt < 2u)\n");
		source.append(s_state.body);
		return source;
	}

	std::vector<u8> CompileSelector(const DKTfxSelector& sel)
	{
#ifdef DK3D_RUNTIME_SHADERS
		const std::string source = BuildSpecializedSource(sel);

		Common::Timer timer;
		size_t dksh_size = 0;
		char* log = nullptr;
		void* dksh = UamCompileGlsl(source.c_str(), UamStage_Fragment, &dksh_size, &log);

		const bool have_log = (log != nullptr && log[0] != '\0');
		if (!dksh || dksh_size == 0)
		{
			Console.Error("DK3D: uam rejected a tfx specialisation:\n%s", have_log ? log : "(no diagnostics)");
			std::free(log);
			std::free(dksh);
			return {};
		}
		if (have_log)
			Console.Warning("DK3D: uam warnings for a tfx specialisation:\n%s", log);
		std::free(log);

		const auto* bytes = static_cast<const u8*>(dksh);
		std::vector<u8> blob(bytes, bytes + dksh_size);
		std::free(dksh);

		s_state.total_compile_ms += timer.GetTimeMilliseconds();
		return blob;
#else
		return {};
#endif
	}

	void WorkerThread()
	{
		std::unique_lock<std::mutex> guard(s_state.lock);
		for (;;)
		{
			s_state.cv.wait(guard, []() { return s_state.shutdown || !s_state.queue.empty(); });
			if (s_state.shutdown)
				break;

			const CompileRequest request = s_state.queue.front();
			s_state.queue.pop_front();

			guard.unlock();
			std::vector<u8> blob = CompileSelector(request.sel);
			if (!blob.empty())
				AppendToDiskCache(request.sel, blob);
			guard.lock();

			if (blob.empty())
				s_state.failed_count++;
			else
				s_state.compiled_count++;

			s_state.results.push_back(CompileResult{request.sel, request.hash, std::move(blob)});
			s_state.in_flight--;
		}
	}
} // namespace

bool DKShaderCompiler::Open()
{
#ifndef DK3D_RUNTIME_SHADERS
	return false;
#else
	if (s_state.open)
		return true;

	if (!LoadShaderSource())
		return false;

	const std::string filename = GetCacheFileName();
	const bool rewrite = LoadDiskCache(filename);
	OpenCacheForAppend(filename, rewrite);

	s_state.shutdown = false;
	s_state.worker = std::thread(WorkerThread);
	s_state.open = true;

	Console.WriteLn("DK3D: runtime tfx shader compilation enabled (%zu cached).", s_state.disk.size());
	return true;
#endif
}

void DKShaderCompiler::Close()
{
	if (!s_state.open)
		return;

	{
		std::lock_guard<std::mutex> guard(s_state.lock);
		s_state.shutdown = true;
		s_state.queue.clear();
	}
	s_state.cv.notify_all();
	if (s_state.worker.joinable())
		s_state.worker.join();

	{
		std::lock_guard<std::mutex> guard(s_state.file_lock);
		if (s_state.file)
		{
			std::fclose(s_state.file);
			s_state.file = nullptr;
		}
	}

	if (s_state.compiled_count > 0)
	{
		Console.WriteLn("DK3D: compiled %u tfx shaders (%u failed) in %.0f ms of worker time.",
			s_state.compiled_count, s_state.failed_count, s_state.total_compile_ms);
	}

	s_state.results.clear();
	s_state.disk.clear();
	s_state.version_line.clear();
	s_state.body.clear();
	s_state.in_flight = 0;
	s_state.compiled_count = 0;
	s_state.failed_count = 0;
	s_state.total_compile_ms = 0.0;
	s_state.open = false;
}

bool DKShaderCompiler::IsOpen()
{
	return s_state.open;
}

u64 DKShaderCompiler::HashSelector(const DKTfxSelector& sel)
{
	return GSXXH3_64bits(&sel, sizeof(sel));
}

void DKShaderCompiler::Request(const DKTfxSelector& sel, u64 hash)
{
	if (!s_state.open)
		return;

	std::lock_guard<std::mutex> guard(s_state.lock);

	// A hit from a previous run skips the compiler entirely.
	const auto it = s_state.disk.find(hash);
	if (it != s_state.disk.end())
	{
		if (it->second.sel == sel)
		{
			s_state.results.push_back(CompileResult{sel, hash, std::move(it->second.blob)});
			s_state.disk.erase(it);
			return;
		}
		s_state.disk.erase(it);
	}

	s_state.queue.push_back(CompileRequest{sel, hash});
	s_state.in_flight++;
	s_state.cv.notify_one();
}

bool DKShaderCompiler::PopCompiled(u64& hash, DKTfxSelector& sel, std::vector<u8>& blob)
{
	if (!s_state.open)
		return false;

	std::lock_guard<std::mutex> guard(s_state.lock);
	if (s_state.results.empty())
		return false;

	CompileResult& result = s_state.results.front();
	hash = result.hash;
	sel = result.sel;
	blob = std::move(result.blob);
	s_state.results.pop_front();
	return true;
}

u32 DKShaderCompiler::GetPendingCount()
{
	if (!s_state.open)
		return 0;

	std::lock_guard<std::mutex> guard(s_state.lock);
	return s_state.in_flight;
}

#endif // __SWITCH__
