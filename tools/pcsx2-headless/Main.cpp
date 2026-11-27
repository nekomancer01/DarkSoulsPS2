// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
// pcsx2-dsrtest: derived from PCSX2 v2.8.2 pcsx2-gsrunner/Main.cpp (headless DarkSoulsPS2 test runner).

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <deque>
#include <functional>
#include <future>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

#include "fmt/format.h"

#include "common/Assertions.h"
#include "common/CocoaTools.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/Renderers/Common/GSDevice.h"
#include "pcsx2/GS/GSPerfMon.h"
#include "pcsx2/GSDumpReplayer.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/VMManager.h"

#include "svnrev.h"

// Down here because X11 has a lot of defines that can conflict
#if defined(__linux__)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace GSRunner
{
	static void InitializeConsole();
	static bool InitializeConfig();
	static void SettingsOverride();
	static bool ParseCommandLineArgs(int argc, char* argv[], VMBootParameters& params);
	static void DumpStats();

	static bool CreatePlatformWindow();
	static void DestroyPlatformWindow();
	static std::optional<WindowInfo> GetPlatformWindowInfo();
	static void PumpPlatformMessages(bool forever = false);
	static void StopPlatformMessagePump();
} // namespace GSRunner

static constexpr u32 WINDOW_WIDTH = 640;
static constexpr u32 WINDOW_HEIGHT = 480;

static MemorySettingsInterface s_settings_interface;
// The base settings layer: s_settings_interface, or with -datapath the ini <datapath>/PCSX2/inis/PCSX2.ini that
// tools/test-pcsx2-ini.py writes. Overrides go into the loaded ini in memory only: the object is never destroyed
// (INISettingsInterface saves itself on destruction when dirty) and Host::CommitBaseSettingChanges does nothing.
static SettingsInterface* s_si = &s_settings_interface;

// pcsx2-dsrtest options (tools/pcsx2-run.sh builds the command line; tools/pcsx2ctl.py speaks the control socket)
static std::string s_datapath;
static std::string s_ctl_path;
static std::string s_bios_dir;
static std::string s_logfile;
static int s_timeout = 0;
static bool s_renderer_set = false;
static bool s_dump_mode = false;

static std::string s_output_prefix;
static s32 s_loop_count = 1;
static std::optional<bool> s_use_window;
static bool s_no_console = false;

// Owned by the GS thread.
static u32 s_dump_frame_number = 0;
static u32 s_loop_number = s_loop_count;
static double s_last_internal_draws = 0;
static double s_last_draws = 0;
static double s_last_render_passes = 0;
static double s_last_barriers = 0;
static double s_last_copies = 0;
static double s_last_uploads = 0;
static double s_last_readbacks = 0;
static double s_last_depth_copies_rov = 0;
static double s_last_draws_rov = 0;
static double s_last_barriers_rov = 0;
static u64 s_total_internal_draws = 0;
static u64 s_total_draws = 0;
static u64 s_total_render_passes = 0;
static u64 s_total_barriers = 0;
static u64 s_total_copies = 0;
static u64 s_total_uploads = 0;
static u64 s_total_readbacks = 0;
static u64 s_total_copies_rov = 0;
static u64 s_total_draws_rov = 0;
static u64 s_total_barriers_rov = 0;
static u32 s_total_frames = 0;
static u32 s_total_drawn_frames = 0;

static bool s_perf_enable = false;
static float s_perf_updates = 0.0f;
static float s_perf_sum_fps = 0.0f;
static float s_perf_sum_internal_fps = 0.0f;
static float s_perf_sum_cpu_thread_usage = 0.0f;
static float s_perf_sum_cpu_thread_time = 0.0f;
static float s_perf_sum_gs_thread_usage = 0.0f;
static float s_perf_sum_gs_thread_time = 0.0f;
static float s_perf_sum_gpu_time = 0.0f;
static float s_perf_sum_gpu_usage = 0.0f;

bool GSRunner::InitializeConfig()
{
	EmuFolders::SetAppRoot();
	// -datapath <dir>: data under <dir>/PCSX2, as pcsx2-qt's -datapath (QtHost.cpp:2216-2220)
	if (!s_datapath.empty())
	{
		EmuConfig.CustomDataPath = s_datapath;
		FileSystem::CreateDirectoryPath(Path::Combine(s_datapath, "PCSX2").c_str(), true);
	}
	if (!EmuFolders::SetResourcesDirectory() || !EmuFolders::SetDataDirectory(nullptr))
		return false;

	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

	const char* error;
	if (!VMManager::PerformEarlyHardwareChecks(&error))
		return false;

	{
		const std::string roboto_path =
			EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
		const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
		if (roboto_data.empty())
		{
			Console.ErrorFmt("Failed to load font file '{}'.", roboto_path);
			return false;
		}

		std::vector<ImGuiManager::FontInfo> fonts;
		ImGuiManager::FontInfo fi{};
		fi.data = roboto_data;
		fi.exclude_ranges = {};
		fi.face_name = nullptr;
		fi.is_emoji_font = false;
		fonts.push_back(fi);

		ImGuiManager::SetFonts(std::move(fonts));
	}

	// Without -datapath: everything in memory (GS dump replays). With it: the data dir's ini, loaded once and never
	// saved (Key Decision 7 of the headless plan).
	const std::string ini_path = Path::Combine(EmuFolders::Settings, "PCSX2.ini");
	if (!s_datapath.empty() && FileSystem::FileExists(ini_path.c_str()))
	{
		INISettingsInterface* ini = new INISettingsInterface(ini_path); // never deleted: see s_si
		if (!ini->Load())
		{
			Console.ErrorFmt("Failed to load {}.", ini_path);
			return false;
		}
		Console.WriteLnFmt("Loading config from {}.", ini_path);
		s_si = ini;
		Host::Internal::SetBaseSettingsLayer(s_si);
	}
	else
	{
		if (!s_datapath.empty())
			Console.WarningFmt("No {}: default settings.", ini_path);
		Host::Internal::SetBaseSettingsLayer(s_si);
		VMManager::SetDefaultSettings(*s_si, true, true, true, true, true);
	}

	// VMManager::Internal::LoadStartupSettings() runs in main() once the arguments and overrides are in
	return true;
}

void Host::CommitBaseSettingChanges()
{
	// nothing to save, we're all in memory
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	// not running any UI, so no settings requests will come in
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
	// nothing
}

bool Host::LocaleCircleConfirm()
{
	// not running any UI, so no settings requests will come in
	return false;
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		INFO_LOG("ReportInfoAsync: {}: {}", title, message);
	else if (!message.empty())
		INFO_LOG("ReportInfoAsync: {}", message);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
	else if (!message.empty())
		ERROR_LOG("ReportErrorAsync: {}", message);
}

void Host::OpenURL(const std::string_view url)
{
	// noop
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

std::string Host::GetTextFromClipboard()
{
	return std::string();
}

void Host::BeginTextInput()
{
	// noop
}

void Host::EndTextInput()
{
	// noop
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	return GSRunner::GetPlatformWindowInfo();
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::SetMouseLock(bool state)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	return GSRunner::GetPlatformWindowInfo();
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
	if (s_loop_number == 0 && !s_output_prefix.empty())
	{
		// when we wrap around, don't race other files
		GSJoinSnapshotThreads();

		// queue dumping of this frame
		std::string dump_path(fmt::format("{}_frame{:05}.png", s_output_prefix, s_dump_frame_number));
		GSQueueSnapshot(dump_path);
	}

	if (GSIsHardwareRenderer())
	{
		const u32 last_draws = s_total_internal_draws;
		const u32 last_uploads = s_total_uploads;

		static constexpr auto update_stat = [](GSPerfMon::counter_t counter, u64& dst, double& last) {
			// perfmon resets every 30 frames to zero
			const double val = g_perfmon.GetCounter(counter);
			dst += static_cast<u64>((val < last) ? val : (val - last));
			last = val;
		};

		update_stat(GSPerfMon::Draw, s_total_internal_draws, s_last_internal_draws);
		update_stat(GSPerfMon::DrawCalls, s_total_draws, s_last_draws);
		update_stat(GSPerfMon::RenderPasses, s_total_render_passes, s_last_render_passes);
		update_stat(GSPerfMon::Barriers, s_total_barriers, s_last_barriers);
		update_stat(GSPerfMon::TextureCopies, s_total_copies, s_last_copies);
		update_stat(GSPerfMon::TextureUploads, s_total_uploads, s_last_uploads);
		update_stat(GSPerfMon::Readbacks, s_total_readbacks, s_last_readbacks);
		update_stat(GSPerfMon::TextureCopiesROV, s_total_copies_rov, s_last_depth_copies_rov);
		update_stat(GSPerfMon::DrawCallsROV, s_total_draws_rov, s_last_draws_rov);
		update_stat(GSPerfMon::BarriersROV, s_total_barriers_rov, s_last_barriers_rov);

		const bool idle_frame = s_total_frames && (last_draws == s_total_internal_draws && last_uploads == s_total_uploads);

		if (!idle_frame)
			s_total_drawn_frames++;

		s_total_frames++;

		std::atomic_thread_fence(std::memory_order_release);
	}
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVMDestroyed()
{
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
}

void Host::OnPerformanceMetricsUpdated()
{
	if (s_perf_enable)
	{
		s_perf_updates += 1.0f;
		s_perf_sum_fps += PerformanceMetrics::GetFPS();
		s_perf_sum_internal_fps += PerformanceMetrics::GetInternalFPS();
		s_perf_sum_cpu_thread_usage += PerformanceMetrics::GetCPUThreadUsage();
		s_perf_sum_cpu_thread_time += PerformanceMetrics::GetCPUThreadAverageTime();
		s_perf_sum_gs_thread_usage += PerformanceMetrics::GetGSThreadUsage();
		s_perf_sum_gs_thread_time += PerformanceMetrics::GetGSThreadAverageTime();
		s_perf_sum_gpu_time += PerformanceMetrics::GetGPUAverageTime();
		s_perf_sum_gpu_usage += PerformanceMetrics::GetGPUUsage();
	}
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
	Console.WriteLnFmt("(dsrtest) state loaded: {} ({})", filename, was_successful ? "ok" : "failed");
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

static std::mutex s_cpu_queue_mutex;
static std::deque<std::function<void()>> s_cpu_queue;

static void DrainCPUQueue()
{
	for (;;)
	{
		std::function<void()> f;
		{
			std::unique_lock lock(s_cpu_queue_mutex);
			if (s_cpu_queue.empty())
				return;
			f = std::move(s_cpu_queue.front());
			s_cpu_queue.pop_front();
		}
		f();
	}
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	std::promise<void> done;
	std::future<void> fut = done.get_future();
	{
		std::unique_lock lock(s_cpu_queue_mutex);
		s_cpu_queue.emplace_back([function = std::move(function), &done, block]() mutable {
			function();
			if (block)
				done.set_value();
		});
	}
	if (block)
		fut.wait();
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
}

void Host::CancelGameListRefresh()
{
}

bool Host::IsFullscreen()
{
	return false;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

void Host::RequestExitApplication(bool allow_confirm)
{
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	VMManager::SetState(VMState::Stopping);
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
	// noop
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
	// noop
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
	// noop
}

void Host::OnAchievementsRefreshed()
{
	// noop
}

bool Host::InBatchMode()
{
	return false;
}

bool Host::InNoGUIMode()
{
	return false;
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	callback(std::string());
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	const int res = std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
	if (res != 0)
		return res;
	return lhs.size() > rhs.size() ? 1 : (lhs.size() < rhs.size() ? -1 : 0);
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

static void PrintCommandLineVersion()
{
	std::fprintf(stderr, "pcsx2-dsrtest (PCSX2 GS Runner) Version %s\n", GIT_REV);
	std::fprintf(stderr, "https://pcsx2.net/\n");
	std::fprintf(stderr, "\n");
}

static void PrintCommandLineHelp(const char* progname)
{
	PrintCommandLineVersion();
	std::fprintf(stderr, "Usage: %s [parameters] [--] [filename]\n", progname);
	std::fprintf(stderr, "\n");
	std::fprintf(stderr, "pcsx2-dsrtest: headless DarkSoulsPS2 test runner (surfaceless; GS dumps, ELFs and disc images).\n");
	std::fprintf(stderr, "  -datapath <dir>: data dir (<dir>/PCSX2: inis/PCSX2.ini is the base settings layer, logs, snaps, sstates, memcards).\n");
	std::fprintf(stderr, "  -elf <file>: boot this ELF (host: = its folder); a file after -- goes into the drive.\n");
	std::fprintf(stderr, "  -gameargs <args>: ELF argv.\n");
	std::fprintf(stderr, "  -statefile <file>: load this savestate after boot.\n");
	std::fprintf(stderr, "  -fastboot: BIOS fast boot (default).\n");
	std::fprintf(stderr, "  -ctl <socket>: control socket (one line in, one line out: shot <png> | gsdump <gs.zst> | save <p2s> | quit).\n");
	std::fprintf(stderr, "  -bios <dir>: BIOS folder (overrides the ini).\n");
	std::fprintf(stderr, "  -timeout <s>: hard wall-clock stop, exit code 3.\n");
	std::fprintf(stderr, "  Exit codes: 0 quit / guest powered off / dump finished, 1 bad arguments or boot failed, 3 timeout.\n");
	std::fprintf(stderr, "  -help: Displays this information and exits.\n");
	std::fprintf(stderr, "  -version: Displays version information and exits.\n");
	std::fprintf(stderr, "  -dumpdir <dir>: Frame dump directory (will be dumped as filename_frameN.png).\n");
	std::fprintf(stderr, "  -dump [rt|tex|z|f|a|i|tr|ds|fs|hw]: Enabling dumping of render target, texture, z buffer, frame, "
		"alphas, and info (context, vertices, list of transfers), transfers images, draw stats, frame stats, HW config, respectively, per draw. Generates lots of data.\n");
	std::fprintf(stderr, "  -dumprange N[,L,B]: Start dumping from draw N (base 0), stops after L draws, and only "
		"those draws that are multiples of B (intersection of -dumprange and -dumprangef used)."
		"Defaults to 0,-1,1 (all draws). Only used if -dump used.\n");
	std::fprintf(stderr, "  -dumprangef NF[,LF,BF]: Start dumping from frame NF (base 0), stops after LF frames, "
		"and only those frames that are multiples of BF (intersection of -dumprange and -dumprangef used).\n"
		"Defaults to 0,-1,1 (all frames). Only used if -dump is used.\n");
	std::fprintf(stderr, "  -loop <count>: Loops dump playback N times. Defaults to 1. 0 will loop infinitely.\n");
	std::fprintf(stderr, "  -renderer <vulkan|opengl|sw>: Sets the graphics renderer. Defaults to vulkan.\n");
	std::fprintf(stderr, "  -swthreads <threads>: Sets the number of threads for the software renderer.\n");
	std::fprintf(stderr, "  -window: Forces a window to be displayed.\n");
	std::fprintf(stderr, "  -surfaceless: Disables showing a window (default).\n");
	std::fprintf(stderr, "  -logfile <filename>: Writes emu log to filename.\n");
	std::fprintf(stderr, "  -noshadercache: Disables the shader cache (useful for parallel runs).\n");
	std::fprintf(stderr, "  -perf: Enable frame timing performance stats.\n");
	std::fprintf(stderr, "  --: Signals that no more arguments will follow and the remaining\n"
						 "    parameters make up the filename. Use when the filename contains\n"
						 "    spaces or starts with a dash.\n");
	std::fprintf(stderr, "\n");
}

void GSRunner::InitializeConsole()
{
	const char* var = std::getenv("PCSX2_NOCONSOLE");
	s_no_console = (var && StringUtil::FromChars<bool>(var).value_or(false));
	if (!s_no_console)
		Log::SetConsoleOutputLevel(LOGLEVEL_DEBUG);
}

bool GSRunner::ParseCommandLineArgs(int argc, char* argv[], VMBootParameters& params)
{
	std::string dumpdir; // Save from argument -dumpdir for creating sub-directories
	bool no_more_args = false;
	for (int i = 1; i < argc; i++)
	{
		if (!no_more_args)
		{
#define CHECK_ARG(str) !std::strcmp(argv[i], str)
#define CHECK_ARG_PARAM(str) (!std::strcmp(argv[i], str) && ((i + 1) < argc))

			if (CHECK_ARG("-help"))
			{
				PrintCommandLineHelp(argv[0]);
				return false;
			}
			else if (CHECK_ARG("-version"))
			{
				PrintCommandLineVersion();
				return false;
			}
			else if (CHECK_ARG_PARAM("-datapath"))
			{
				++i; // read in main() before the config is initialised
				continue;
			}
			else if (CHECK_ARG_PARAM("-elf"))
			{
				params.elf_override = Path::RealPath(argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("-gameargs"))
			{
				EmuConfig.CurrentGameArgs = argv[++i];
				continue;
			}
			else if (CHECK_ARG_PARAM("-statefile"))
			{
				params.save_state = Path::RealPath(argv[++i]);
				continue;
			}
			else if (CHECK_ARG("-fastboot"))
			{
				params.fast_boot = true;
				continue;
			}
			else if (CHECK_ARG_PARAM("-ctl"))
			{
				s_ctl_path = argv[++i];
				continue;
			}
			else if (CHECK_ARG_PARAM("-bios"))
			{
				s_bios_dir = Path::RealPath(argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("-timeout"))
			{
				s_timeout = StringUtil::FromChars<int>(argv[++i]).value_or(-1);
				if (s_timeout <= 0)
				{
					Console.Error("Invalid -timeout.");
					return false;
				}
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumpdir"))
			{
				dumpdir = s_output_prefix = StringUtil::StripWhitespace(argv[++i]);
				if (s_output_prefix.empty())
				{
					Console.Error("Invalid dump directory specified.");
					return false;
				}

				if (!FileSystem::DirectoryExists(s_output_prefix.c_str()) && !FileSystem::CreateDirectoryPath(s_output_prefix.c_str(), false))
				{
					Console.Error("Failed to create output directory");
					return false;
				}

				continue;
			}
			else if (CHECK_ARG_PARAM("-dump"))
			{
				std::string str(argv[++i]);

				s_si->SetBoolValue("EmuCore/GS", "DumpGSData", true);

				if (str.find("rt") != std::string::npos)
					s_si->SetBoolValue("EmuCore/GS", "SaveRT", true);
				if (str.find("f") != std::string::npos)
					s_si->SetBoolValue("EmuCore/GS", "SaveFrame", true);
				if (str.find("tex") != std::string::npos)
					s_si->SetBoolValue("EmuCore/GS", "SaveTexture", true);
				if (str.find("z") != std::string::npos)
					s_si->SetBoolValue("EmuCore/GS", "SaveDepth", true);
				if (str.find("a") != std::string::npos)
					s_si->SetBoolValue("EmuCore/GS", "SaveAlpha", true);
				if (str.find("i") != std::string::npos)
					s_si->SetBoolValue("EmuCore/GS", "SaveInfo", true);
				if (str.find("tr") != std::string::npos)
					s_si->SetBoolValue("EmuCore/GS", "SaveTransferImages", true);
				if (str.find("ds") != std::string::npos)
					s_si->SetBoolValue("EmuCore/GS", "SaveDrawStats", true);
				if (str.find("fs") != std::string::npos)
					s_si->SetBoolValue("EmuCore/GS", "SaveFrameStats", true);
				if (str.find("hw") != std::string::npos)
					s_si->SetBoolValue("EmuCore/GS", "SaveHWConfig", true);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumprange"))
			{
				std::string str(argv[++i]);

				std::vector<std::string_view> split = StringUtil::SplitString(str, ',');
				int start = 0;
				int num = -1;
				int by = 1;
				if (split.size() > 0)
				{
					start = StringUtil::FromChars<int>(split[0]).value_or(0);
				}
				if (split.size() > 1)
				{
					num = StringUtil::FromChars<int>(split[1]).value_or(-1);
				}
				if (split.size() > 2)
				{
					by = std::max(1, StringUtil::FromChars<int>(split[2]).value_or(1));
				}
				s_si->SetIntValue("EmuCore/GS", "SaveDrawStart", start);
				s_si->SetIntValue("EmuCore/GS", "SaveDrawCount", num);
				s_si->SetIntValue("EmuCore/GS", "SaveDrawBy", by);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumprangef"))
			{
				std::string str(argv[++i]);

				std::vector<std::string_view> split = StringUtil::SplitString(str, ',');
				int start = 0;
				int num = -1;
				int by = 1;
				if (split.size() > 0)
				{
					start = StringUtil::FromChars<int>(split[0]).value_or(0);
				}
				if (split.size() > 1)
				{
					num = StringUtil::FromChars<int>(split[1]).value_or(-1);
				}
				if (split.size() > 2)
				{
					by = std::max(1, StringUtil::FromChars<int>(split[2]).value_or(1));
				}
				s_si->SetIntValue("EmuCore/GS", "SaveFrameStart", start);
				s_si->SetIntValue("EmuCore/GS", "SaveFrameCount", num);
				s_si->SetIntValue("EmuCore/GS", "SaveFrameBy", by);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumpdirhw"))
			{
				s_si->SetStringValue("EmuCore/GS", "HWDumpDirectory", argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumpdirsw"))
			{
				s_si->SetStringValue("EmuCore/GS", "SWDumpDirectory", argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("-loop"))
			{
				s_loop_count = StringUtil::FromChars<s32>(argv[++i]).value_or(0);
				Console.WriteLn("Looping dump playback %d times.", s_loop_count);
				continue;
			}
			else if (CHECK_ARG_PARAM("-renderer"))
			{
				const char* rname = argv[++i];

				GSRendererType type = GSRendererType::Auto;
				if (StringUtil::Strcasecmp(rname, "Auto") == 0)
					type = GSRendererType::Auto;
#ifdef _WIN32
				else if (StringUtil::Strcasecmp(rname, "dx11") == 0)
					type = GSRendererType::DX11;
				else if (StringUtil::Strcasecmp(rname, "dx12") == 0)
					type = GSRendererType::DX12;
#endif
#ifdef ENABLE_OPENGL
				else if (StringUtil::Strcasecmp(rname, "gl") == 0 || StringUtil::Strcasecmp(rname, "opengl") == 0)
					type = GSRendererType::OGL;
#endif
#ifdef ENABLE_VULKAN
				else if (StringUtil::Strcasecmp(rname, "vulkan") == 0)
					type = GSRendererType::VK;
#endif
#ifdef __APPLE__
				else if (StringUtil::Strcasecmp(rname, "metal") == 0)
					type = GSRendererType::Metal;
#endif
				else if (StringUtil::Strcasecmp(rname, "sw") == 0)
					type = GSRendererType::SW;
				else
				{
					Console.Error("Unknown renderer '%s'", rname);
					return false;
				}

				Console.WriteLn("Using %s renderer.", Pcsx2Config::GSOptions::GetRendererName(type));
				s_si->SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(type));
				s_renderer_set = true;
				continue;
			}
			else if (CHECK_ARG_PARAM("-swthreads"))
			{
				const int swthreads = StringUtil::FromChars<int>(argv[++i]).value_or(0);
				if (swthreads < 0)
				{
					Console.WriteLn("Invalid number of software threads");
					return false;
				}
				
				Console.WriteLn(fmt::format("Setting number of software threads to {}", swthreads));
				s_si->SetIntValue("EmuCore/GS", "SWExtraThreads", swthreads);
				continue;
			}
			else if (CHECK_ARG_PARAM("-renderhacks"))
			{
				std::string str(argv[++i]);

				s_si->SetBoolValue("EmuCore/GS", "UserHacks", true);

				if (str.find("af") != std::string::npos)
					s_si->SetIntValue("EmuCore/GS", "UserHacks_AutoFlushLevel", 1);
				if (str.find("cpufb") != std::string::npos)
					s_si->SetBoolValue("EmuCore/GS", "UserHacks_CPU_FB_Conversion", true);
				if (str.find("dds") != std::string::npos)
					s_si->SetBoolValue("EmuCore/GS", "UserHacks_DisableDepthSupport", true);
				if (str.find("dpi") != std::string::npos)
					s_si->SetBoolValue("EmuCore/GS", "UserHacks_DisablePartialInvalidation", true);
				if (str.find("dsf") != std::string::npos)
					s_si->SetBoolValue("EmuCore/GS", "UserHacks_Disable_Safe_Features", true);
				if (str.find("tinrt") != std::string::npos)
					s_si->SetIntValue("EmuCore/GS", "UserHacks_TextureInsideRt", 1);
				if (str.find("plf") != std::string::npos)
					s_si->SetBoolValue("EmuCore/GS", "preload_frame_with_gs_data", true);

				continue;
			}
			else if (CHECK_ARG_PARAM("-ini"))
			{
				std::string path = std::string(StringUtil::StripWhitespace(argv[++i]));
				if (!FileSystem::FileExists(path.c_str()))
				{
					Console.ErrorFmt("INI file {} does not exit.", path);
					return false;
				}

				INISettingsInterface si_ini(path);

				if (!si_ini.Load())
				{
					Console.ErrorFmt("Unable to load INI settings from {}.", path);
					return false;
				}

				for (const auto& [key, value] : si_ini.GetKeyValueList("EmuCore/GS"))
					s_si->SetStringValue("EmuCore/GS", key.c_str(), value.c_str());

				continue;
			}
			else if (CHECK_ARG_PARAM("-upscale"))
			{
				const float upscale = StringUtil::FromChars<float>(argv[++i]).value_or(0.0f);
				if (upscale < 0.5f)
				{
					Console.WriteLn("Invalid upscale multiplier");
					return false;
				}

				Console.WriteLn(fmt::format("Setting upscale multiplier to {}", upscale));
				s_si->SetFloatValue("EmuCore/GS", "upscale_multiplier", upscale);
				continue;
			}
			else if (CHECK_ARG_PARAM("-logfile"))
			{
				const char* logfile = argv[++i];
				if (std::strlen(logfile) > 0)
				{
					// disable timestamps, since we want to be able to diff the logs
					Console.WriteLn("Logging to %s...", logfile);
					s_logfile = logfile;
					VMManager::Internal::SetFileLogPath(logfile);
					s_si->SetBoolValue("Logging", "EnableFileLogging", true);
					s_si->SetBoolValue("Logging", "EnableTimestamps", false);
				}

				continue;
			}
			else if (CHECK_ARG("-noshadercache"))
			{
				Console.WriteLn("Disabling shader cache");
				s_si->SetBoolValue("EmuCore/GS", "DisableShaderCache", true);
				continue;
			}
			else if (CHECK_ARG("-window"))
			{
				Console.WriteLn("Creating window");
				s_use_window = true;
				continue;
			}
			else if (CHECK_ARG("-surfaceless"))
			{
				Console.WriteLn("Running surfaceless");
				s_use_window = false;
				continue;
			}
			else if (CHECK_ARG("-perf"))
			{
				Console.WriteLn("Enable performance stats");
				s_perf_enable = true;
				continue;
			}
			else if (CHECK_ARG("-debugdevice"))
			{
				Console.WriteLn("Enable debug device");
				s_si->SetBoolValue("EmuCore/GS", "UseDebugDevice", true);
				continue;
			}
			else if (CHECK_ARG("--"))
			{
				no_more_args = true;
				continue;
			}
			else if (argv[i][0] == '-')
			{
				Console.Error("Unknown parameter (or missing value): '%s'", argv[i]);
				return false;
			}

#undef CHECK_ARG
#undef CHECK_ARG_PARAM
		}

		if (!params.filename.empty())
			params.filename += ' ';
		params.filename += argv[i];
	}

	if (params.filename.empty() && params.elf_override.empty())
	{
		Console.Error("Nothing to boot: give -elf <file> and/or a disc image or GS dump after --.");
		return false;
	}

	// a GS dump replays as in pcsx2-gsrunner; anything else boots (ELF via -elf, disc image in the drive / booted)
	s_dump_mode = !params.filename.empty() && VMManager::IsGSDumpFileName(params.filename);
	if (!params.filename.empty())
	{
		params.filename = Path::RealPath(params.filename);
		if (!FileSystem::FileExists(params.filename.c_str()))
		{
			Console.ErrorFmt("No such file: {}", params.filename);
			return false;
		}
	}
	if (!params.elf_override.empty() && !FileSystem::FileExists(params.elf_override.c_str()))
	{
		Console.ErrorFmt("No such ELF: {}", params.elf_override);
		return false;
	}
	if (!s_dump_mode)
		s_output_prefix.clear(); // -dumpdir frame dumps are for GS dump replays only

	if (s_si->GetBoolValue("EmuCore/GS", "DumpGSData") && !dumpdir.empty())
	{
		if (s_si->GetStringValue("EmuCore/GS", "HWDumpDirectory").empty())
			s_si->SetStringValue("EmuCore/GS", "HWDumpDirectory", dumpdir.c_str());
		if (s_si->GetStringValue("EmuCore/GS", "SWDumpDirectory").empty())
			s_si->SetStringValue("EmuCore/GS", "SWDumpDirectory", dumpdir.c_str());
		
		// Disable saving frames with SaveSnapshotToMemory()
		// Instead we save more "raw" snapshots when using -dump.
		s_output_prefix = "";
	}

	// set up the frame dump directory
	if (s_dump_mode && !s_output_prefix.empty())
	{
		// strip off all extensions
		std::string_view title(Path::GetFileTitle(params.filename));
		if (StringUtil::EndsWithNoCase(title, ".gs"))
			title = Path::GetFileTitle(title);

		s_output_prefix = Path::Combine(s_output_prefix, StringUtil::StripWhitespace(title));
		Console.WriteLn(fmt::format("Saving dumps as {}_frameN.png", s_output_prefix));
	}

	return true;
}

void GSRunner::SettingsOverride()
{
	// Speed comes from the ini (tools/test-pcsx2-ini.py TURBO keys). GS dump replays always run unlimited.
	if (s_datapath.empty() || s_dump_mode)
		s_si->SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
	s_si->SetIntValue("EmuCore/GS", "VsyncEnable", false);

	// default renderer: Vulkan surfaceless (-renderer opengl is the fallback, sw for dump replays)
	if (!s_renderer_set)
		s_si->SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::VK));

	// screenshots (control socket "shot") at internal resolution: there is no window to size them by
	s_si->SetIntValue("EmuCore/GS", "ScreenshotSize", static_cast<int>(GSScreenshotSize::InternalResolution));

	// tests read their scripts and write saves through host: (HostFs), and boot through the BIOS fast boot
	if (!s_si->ContainsValue("EmuCore", "HostFs")) // an ini may turn it off (tools/ps2-pcsx2.sh HOSTFS=0)
		s_si->SetBoolValue("EmuCore", "HostFs", true);
	s_si->SetBoolValue("EmuCore", "EnableFastBoot", true);
	if (!s_bios_dir.empty())
		s_si->SetStringValue("Folders", "Bios", s_bios_dir.c_str());

	// Force screenshot quality settings to something more performant, overriding any defaults good for users.
	s_si->SetIntValue("EmuCore/GS", "ScreenshotFormat", static_cast<int>(GSScreenshotFormat::PNG));
	s_si->SetIntValue("EmuCore/GS", "ScreenshotQuality", 10);

	// ensure all input sources are disabled, we're not using them
	s_si->SetBoolValue("InputSources", "SDL", false);
	s_si->SetBoolValue("InputSources", "XInput", false);

	// we don't need any sound output
	// (pcsx2-gsrunner sets the pre-2.x key OutputModule = nullout, which 2.8.2 no longer reads: its runs opened a cubeb
	// stream. The 2.x key is Backend, Pcsx2Config.cpp:1272 / AudioStream.cpp s_backend_names.)
	s_si->SetStringValue("SPU2/Output", "Backend", "Null");

	// none of the bindings are going to resolve to anything
	Pad::ClearPortBindings(*s_si, 0);
	s_si->ClearSection("Hotkeys");

	// force logging (no timestamps in a -logfile: the harnesses read its lines as the ELF printed them)
	s_si->SetBoolValue("Logging", "EnableSystemConsole", !s_no_console);
	s_si->SetBoolValue("Logging", "EnableTimestamps", s_logfile.empty());
	s_si->SetBoolValue("Logging", "EnableVerbose", true);
	// the ELF's printf (its TEST lines, which every harness parses) and the IOP modules' output reach the log only through
	// these; PCSX2 defaults both off, so a tester's ini without them gave runs with no TEST line at all
	s_si->SetBoolValue("Logging", "EnableEEConsole", true);
	s_si->SetBoolValue("Logging", "EnableIOPConsole", true);

	// no OSD: screenshots must show the game's frame only
	s_si->SetBoolValue("EmuCore/GS", "OsdShowFPS", false);
	s_si->SetBoolValue("EmuCore/GS", "OsdShowResolution", false);
	s_si->SetBoolValue("EmuCore/GS", "OsdShowGSStats", false);

	// memory cards as the ini says (ps2/proto/tests/save_roundtrip.sh needs Mcd001.ps2); without any [MemoryCards]
	// keys, slot 1 holds Mcd001.ps2 in the data dir's memcards/
	if (s_dump_mode)
	{
		// dump replays touch no card (as pcsx2-gsrunner: no sharing violations between parallel replays)
		for (u32 i = 0; i < 2; i++)
		{
			s_si->SetBoolValue("MemoryCards", fmt::format("Slot{}_Enable", i + 1).c_str(), false);
			s_si->SetStringValue("MemoryCards", fmt::format("Slot{}_Filename", i + 1).c_str(), "");
		}
	}
	else if (s_si->GetKeyValueList("MemoryCards").empty())
	{
		s_si->SetBoolValue("MemoryCards", "Slot1_Enable", true);
		s_si->SetStringValue("MemoryCards", "Slot1_Filename", "Mcd001.ps2");
		s_si->SetBoolValue("MemoryCards", "Slot2_Enable", false);
	}
}

void GSRunner::DumpStats()
{
	std::atomic_thread_fence(std::memory_order_acquire);
	Console.WriteLn(fmt::format("======= HW STATISTICS FOR {} ({}) FRAMES ========", s_total_frames, s_total_drawn_frames));
	Console.WriteLn(fmt::format("@HWSTAT@ Draw Calls: {} (avg {})", s_total_draws, static_cast<u64>(std::ceil(s_total_draws / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Render Passes: {} (avg {})", s_total_render_passes, static_cast<u64>(std::ceil(s_total_render_passes / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Barriers: {} (avg {})", s_total_barriers, static_cast<u64>(std::ceil(s_total_barriers / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Copies: {} (avg {})", s_total_copies, static_cast<u64>(std::ceil(s_total_copies / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Uploads: {} (avg {})", s_total_uploads, static_cast<u64>(std::ceil(s_total_uploads / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Readbacks: {} (avg {})", s_total_readbacks, static_cast<u64>(std::ceil(s_total_readbacks / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Copies (ROV): {} (avg {})", s_total_copies_rov, static_cast<u64>(std::ceil(s_total_copies_rov / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Draws Calls (ROV): {} (avg {})", s_total_draws_rov, static_cast<u64>(std::ceil(s_total_draws_rov / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Barriers (ROV): {} (avg {})", s_total_barriers_rov, static_cast<u64>(std::ceil(s_total_barriers_rov / static_cast<double>(s_total_drawn_frames)))));
	if (s_perf_enable)
	{
		Console.WriteLn(fmt::format("@HWSTAT@ Minimum Frame Time: {:.3f} ms ({:.3f} FPS)", PerformanceMetrics::GetMinimumFrameTime(), 1000.0f / PerformanceMetrics::GetMinimumFrameTime()));
		Console.WriteLn(fmt::format("@HWSTAT@ Average Frame Time: {:.3f} ms ({:.3f} FPS)", PerformanceMetrics::GetAverageFrameTime(), 1000.0f / PerformanceMetrics::GetAverageFrameTime()));
		Console.WriteLn(fmt::format("@HWSTAT@ Maximum Frame Time: {:.3f} ms ({:.3f} FPS)", PerformanceMetrics::GetMaximumFrameTime(), 1000.0f / PerformanceMetrics::GetMaximumFrameTime()));
		Console.WriteLn(fmt::format("@HWSTAT@ CPU Thread Usage: {:.3f} %", s_perf_sum_cpu_thread_usage / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ GS Thread Usage: {:.3f} %", s_perf_sum_gs_thread_usage / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ GPU Usage: {:.3f} %", s_perf_sum_gpu_usage / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ Average CPU Thread Time: {:.3f} ms", s_perf_sum_cpu_thread_time / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ Average GS Thread Time: {:.3f} ms", s_perf_sum_gs_thread_time / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ Average GPU Time: {:.3f} ms", s_perf_sum_gpu_time / s_perf_updates));
	}
	Console.WriteLn("============================================");
}

//////////////////////////////////////////////////////////////////////////
// Control socket (-ctl <path>): one request line, one reply line, then the server closes the connection.
//   shot <png>          -> GSQueueSnapshot(png, 0) on the GS thread; "ok <png>" once the file is written
//   gsdump <out.gs.zst> -> single-frame GS dump (GSQueueSnapshot(<tmp>.png, 1)), renamed to <out.gs.zst>
//   save <file.p2s>     -> VMManager::SaveState on the CPU thread
//   quit                -> "ok", then the VM stops and the process exits 0
// Errors reply "err <message>". The CPU thread runs the requests at its next vsync (Host::PumpMessagesOnCPUThread);
// the reply waits for the file, since the snapshot is written on the GS thread / its compression thread.
//////////////////////////////////////////////////////////////////////////

#if defined(__linux__)
static int s_ctl_fd = -1;
static std::atomic<bool> s_ctl_stop{false};
static std::atomic<u32> s_ctl_seq{0}; // per-request scratch names (see shot / gsdump)

// a file that exists with the same non-zero size over 10 polls 10 ms apart (100 ms: a GS / compression thread write
// stalled under load must not pass as a finished file)
static bool WaitForStableFile(const std::string& path, int timeout_ms)
{
	s64 last = -1;
	int same = 0;
	for (int t = 0; t < timeout_ms; t += 10)
	{
		const s64 size = FileSystem::GetPathFileSize(path.c_str());
		if (size > 0 && size == last)
		{
			if (++same >= 10)
				return true;
		}
		else
			same = 0;
		last = size;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

// runs f on the CPU thread at its next vsync; false when the VM did not get to it within timeout_ms
static bool RunOnCPUThreadWait(std::function<void()> f, int timeout_ms)
{
	auto done = std::make_shared<std::promise<void>>();
	std::future<void> fut = done->get_future();
	{
		std::unique_lock lock(s_cpu_queue_mutex);
		s_cpu_queue.emplace_back([f = std::move(f), done]() { f(); done->set_value(); });
	}
	return fut.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::ready;
}

static std::string HandleCtlRequest(const std::string& line)
{
	const size_t sp = line.find(' ');
	const std::string verb = line.substr(0, sp);
	const std::string arg = (sp == std::string::npos) ? std::string() : std::string(StringUtil::StripWhitespace(line.substr(sp + 1)));
	static constexpr int WAIT_MS = 20000;

	if (verb == "quit")
	{
		Host::RunOnCPUThread([]() {
			if (VMManager::GetState() == VMState::Running || VMManager::GetState() == VMState::Paused)
				VMManager::SetState(VMState::Stopping);
		});
		return "ok";
	}
	if (arg.empty())
		return "err missing path";
	if (verb == "shot")
	{
		if (!StringUtil::EndsWithNoCase(arg, ".png"))
			return "err shot path must end in .png";
		// written under a scratch name unique to this request, then renamed: a request that timed out stays queued and
		// may still write later, and must never land on (or race) a later request's file
		const std::string tmp = arg.substr(0, arg.size() - 4) + fmt::format(".dsrtest{}-{}.png", getpid(), s_ctl_seq++);
		FileSystem::DeleteFilePath(arg.c_str());
		if (!RunOnCPUThreadWait([tmp]() { MTGS::RunOnGSThread([tmp]() { GSQueueSnapshot(tmp, 0); }); }, WAIT_MS))
			return "err timeout (VM not running)";
		if (!WaitForStableFile(tmp, WAIT_MS))
			return "err timeout";
		return FileSystem::RenamePath(tmp.c_str(), arg.c_str()) ? ("ok " + arg) : "err rename failed";
	}
	if (verb == "gsdump")
	{
		if (!StringUtil::EndsWithNoCase(arg, ".gs.zst"))
			return "err gsdump path must end in .gs.zst";
		// the GS writes <base>.gs.zst plus a screenshot <base>.png: use a scratch base so a shot of the same name
		// next to the dump is not overwritten
		const std::string base = arg.substr(0, arg.size() - 7) + fmt::format(".dsrtest{}-{}", getpid(), s_ctl_seq++);
		const std::string tmp_dump = base + ".gs.zst", tmp_png = base + ".png";
		FileSystem::DeleteFilePath(tmp_dump.c_str());
		FileSystem::DeleteFilePath(tmp_png.c_str());
		FileSystem::DeleteFilePath(arg.c_str());
		if (!RunOnCPUThreadWait([tmp_png]() { MTGS::RunOnGSThread([tmp_png]() { GSQueueSnapshot(tmp_png, 1); }); }, WAIT_MS))
			return "err timeout (VM not running)";
		// the dump is closed one frame after the screenshot of its frame: wait for both
		const bool ok = WaitForStableFile(tmp_png, WAIT_MS) && WaitForStableFile(tmp_dump, WAIT_MS);
		FileSystem::DeleteFilePath(tmp_png.c_str());
		if (!ok)
			return "err timeout";
		if (!FileSystem::RenamePath(tmp_dump.c_str(), arg.c_str()))
			return "err rename failed";
		return "ok " + arg;
	}
	if (verb == "save")
	{
		auto err = std::make_shared<std::string>();
		FileSystem::DeleteFilePath(arg.c_str());
		if (!RunOnCPUThreadWait([arg, err]() {
				if (!VMManager::HasValidVM())
					*err = "no VM";
				else
					VMManager::SaveState(arg.c_str(), false, false, [err](const std::string& e) { *err = e; });
			}, 60000))
			return "err timeout (VM not running)";
		if (!err->empty())
			return "err " + *err;
		return FileSystem::FileExists(arg.c_str()) ? ("ok " + arg) : "err state not written";
	}
	return "err unknown request: " + verb;
}

static void CtlThreadMain()
{
	while (!s_ctl_stop.load())
	{
		pollfd pfd = {s_ctl_fd, POLLIN, 0};
		if (poll(&pfd, 1, 200) <= 0)
			continue;
		const int c = accept(s_ctl_fd, nullptr, nullptr);
		if (c < 0)
			continue;
		std::string line;
		char ch;
		pollfd cfd = {c, POLLIN, 0};
		while (line.size() < 4096 && poll(&cfd, 1, 5000) > 0 && read(c, &ch, 1) == 1 && ch != '\n')
			line += ch;
		const std::string reply = HandleCtlRequest(std::string(StringUtil::StripWhitespace(line))) + "\n";
		Console.WriteLnFmt("(dsrtest ctl) {} -> {}", line, StringUtil::StripWhitespace(reply));
		[[maybe_unused]] const ssize_t w = write(c, reply.data(), reply.size());
		close(c);
	}
}

static bool StartCtlServer()
{
	if (s_ctl_path.empty())
		return true;
	sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	if (s_ctl_path.size() >= sizeof(addr.sun_path))
	{
		Console.ErrorFmt("Control socket path too long: {}", s_ctl_path);
		return false;
	}
	std::strcpy(addr.sun_path, s_ctl_path.c_str());
	unlink(s_ctl_path.c_str());
	s_ctl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (s_ctl_fd < 0 || bind(s_ctl_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s_ctl_fd, 4) != 0)
	{
		Console.ErrorFmt("Control socket {} failed: {}", s_ctl_path, std::strerror(errno));
		return false;
	}
	std::thread(CtlThreadMain).detach();
	Console.WriteLnFmt("Control socket: {}", s_ctl_path);
	return true;
}

static void StopCtlServer()
{
	s_ctl_stop.store(true);
	if (s_ctl_fd >= 0)
	{
		close(s_ctl_fd);
		s_ctl_fd = -1;
		unlink(s_ctl_path.c_str());
	}
}
#else
static bool StartCtlServer() { return s_ctl_path.empty(); }
static void StopCtlServer() {}
#endif

// -timeout <s>: a hard wall-clock stop for an emulator whose harness died (exit 3, nothing is flushed or saved)
static void StartTimeoutWatchdog()
{
	if (s_timeout <= 0)
		return;
	std::thread([]() {
		std::this_thread::sleep_for(std::chrono::seconds(s_timeout));
		std::fprintf(stderr, "pcsx2-dsrtest: -timeout %d s reached, exiting\n", s_timeout);
		if (!s_ctl_path.empty())
			unlink(s_ctl_path.c_str());
		std::_Exit(3);
	}).detach();
}

#ifdef _WIN32
// We can't handle unicode in filenames if we don't use wmain on Win32.
#define main real_main
#endif

static void CPUThreadMain(VMBootParameters* params, std::atomic<int>* ret)
{
	ret->store(EXIT_FAILURE);

	if (VMManager::Internal::CPUThreadInitialize())
	{
		// apply new settings (e.g. pick up renderer change)
		VMManager::ApplySettings();
		GSDumpReplayer::SetIsDumpRunner(s_dump_mode);

		if (VMManager::Initialize(*params) == VMBootResult::StartupSuccess)
		{
			// run until end
			GSDumpReplayer::SetLoopCount(s_loop_count);
			VMManager::SetState(VMState::Running);
			if (s_perf_enable)
			{
				VMManager::SetLimiterMode(LimiterModeType::Unlimited);
				g_gs_device->SetGPUTimingEnabled(true);
			}
			while (VMManager::GetState() == VMState::Running)
			{
				VMManager::Execute();
				DrainCPUQueue();
			}
			VMManager::Shutdown(false);
			GSRunner::DumpStats();
			ret->store(EXIT_SUCCESS);
		}
	}

	VMManager::Internal::CPUThreadShutdown();
	GSRunner::StopPlatformMessagePump();
}

int main(int argc, char* argv[])
{
	CrashHandler::Install();
	GSRunner::InitializeConsole();

	// -datapath has to be known before the data directory is resolved
	for (int i = 1; i + 1 < argc; i++)
	{
		if (!std::strcmp(argv[i], "--"))
			break;
		if (!std::strcmp(argv[i], "-datapath"))
			s_datapath = Path::RealPath(argv[i + 1]);
	}

	if (!GSRunner::InitializeConfig())
	{
		Console.Error("Failed to initialize config.");
		return EXIT_FAILURE;
	}

	VMBootParameters params;
	if (!GSRunner::ParseCommandLineArgs(argc, argv, params))
		return EXIT_FAILURE;

	if (s_use_window.value_or(false) && !GSRunner::CreatePlatformWindow())
	{
		Console.Error("Failed to create window.");
		return EXIT_FAILURE;
	}

	// Override settings that shouldn't be picked up from defaults or INIs.
	GSRunner::SettingsOverride();
	VMManager::Internal::LoadStartupSettings(); // folders (from -datapath / the ini) + the file log
	// the forced test settings, for the log (PCSX2 itself prints no settings dump)
	for (const char* key : {"Renderer", "ScreenshotSize", "OsdShowFPS", "OsdShowResolution", "OsdShowGSStats", "VsyncEnable"})
		Console.WriteLnFmt("(dsrtest) EmuCore/GS {} = {}", key, s_si->GetStringValue("EmuCore/GS", key));
	Console.WriteLnFmt("(dsrtest) SPU2/Output Backend = {}, EmuCore HostFs = {}, MemoryCards Slot1_Enable = {}",
		s_si->GetStringValue("SPU2/Output", "Backend"), s_si->GetStringValue("EmuCore", "HostFs"),
		s_si->GetStringValue("MemoryCards", "Slot1_Enable"));

	StartTimeoutWatchdog();
	if (!StartCtlServer())
		return EXIT_FAILURE;

	std::atomic<int> thread_ret;
	std::thread cputhread(CPUThreadMain, &params, &thread_ret);
	GSRunner::PumpPlatformMessages(/*forever=*/true);
	cputhread.join();

	GSRunner::DestroyPlatformWindow();
	StopCtlServer();

	return thread_ret.load();
}

void Host::PumpMessagesOnCPUThread()
{
	DrainCPUQueue();

	// update GS thread copy of frame number
	MTGS::RunOnGSThread([frame_number = GSDumpReplayer::GetFrameNumber()]() { s_dump_frame_number = frame_number; });
	MTGS::RunOnGSThread([loop_number = GSDumpReplayer::GetLoopCount()]() { s_loop_number = loop_number; });
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	else if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	TinyString count_str = TinyString::from_format("{}", count);

	std::string ret(msg);
	for (;;)
	{
		std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;

		ret.replace(pos, pos + 2, count_str.view());
	}

	return ret;
}

//////////////////////////////////////////////////////////////////////////
// Platform specific code
//////////////////////////////////////////////////////////////////////////

#ifdef _WIN32

static constexpr LPCWSTR WINDOW_CLASS_NAME = L"PCSX2GSRunner";
static HWND s_hwnd = NULL;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool GSRunner::CreatePlatformWindow()
{
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(WNDCLASSEXW);
	wc.style = 0;
	wc.lpfnWndProc = WndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = GetModuleHandle(nullptr);
	wc.hIcon = NULL;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszMenuName = NULL;
	wc.lpszClassName = WINDOW_CLASS_NAME;
	wc.hIconSm = NULL;

	if (!RegisterClassExW(&wc))
	{
		Console.Error("Window registration failed.");
		return false;
	}

	s_hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, WINDOW_CLASS_NAME, L"PCSX2 GS Runner",
		WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_MINIMIZEBOX | WS_SYSMENU | WS_SIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, WINDOW_WIDTH,
		WINDOW_HEIGHT, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
	if (!s_hwnd)
	{
		Console.Error("CreateWindowEx failed.");
		return false;
	}

	ShowWindow(s_hwnd, SW_SHOW);
	UpdateWindow(s_hwnd);

	// make sure all messages are processed before returning
	PumpPlatformMessages();
	return true;
}

void GSRunner::DestroyPlatformWindow()
{
	if (!s_hwnd)
		return;

	PumpPlatformMessages();
	DestroyWindow(s_hwnd);
	s_hwnd = {};
}

std::optional<WindowInfo> GSRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;

	if (s_hwnd)
	{
		RECT rc = {};
		GetWindowRect(s_hwnd, &rc);
		wi.surface_width = static_cast<u32>(rc.right - rc.left);
		wi.surface_height = static_cast<u32>(rc.bottom - rc.top);
		wi.surface_scale = 1.0f;
		wi.type = WindowInfo::Type::Win32;
		wi.window_handle = s_hwnd;
	}
	else
	{
		wi.type = WindowInfo::Type::Surfaceless;
	}

	return wi;
}

static constexpr int SHUTDOWN_MSG = WM_APP + 0x100;
static DWORD MainThreadID;

void GSRunner::PumpPlatformMessages(bool forever)
{
	MSG msg;
	while (true)
	{
		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
		{
			if (msg.message == SHUTDOWN_MSG)
				return;
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (!forever)
			return;
		WaitMessage();
	}
}

void GSRunner::StopPlatformMessagePump()
{
	PostThreadMessageW(MainThreadID, SHUTDOWN_MSG, 0, 0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int wmain(int argc, wchar_t** argv)
{
	std::vector<std::string> u8_args;
	u8_args.reserve(static_cast<size_t>(argc));
	for (int i = 0; i < argc; i++)
		u8_args.push_back(StringUtil::WideStringToUTF8String(argv[i]));

	std::vector<char*> u8_argptrs;
	u8_argptrs.reserve(u8_args.size());
	for (int i = 0; i < argc; i++)
		u8_argptrs.push_back(u8_args[i].data());
	u8_argptrs.push_back(nullptr);

	MainThreadID = GetCurrentThreadId();

	return real_main(argc, u8_argptrs.data());
}

#elif defined(__APPLE__)

static void* s_window;
static WindowInfo s_wi;

bool GSRunner::CreatePlatformWindow()
{
	pxAssertRel(!s_window, "Tried to create window when there already was one!");
	s_window = CocoaTools::CreateWindow("PCSX2 GS Runner", WINDOW_WIDTH, WINDOW_HEIGHT);
	CocoaTools::GetWindowInfoFromWindow(&s_wi, s_window);
	PumpPlatformMessages();
	return s_window;
}

void GSRunner::DestroyPlatformWindow()
{
	if (s_window) {
		CocoaTools::DestroyWindow(s_window);
		s_window = nullptr;
	}
}

std::optional<WindowInfo> GSRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;
	if (s_window)
		wi = s_wi;
	else
		wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void GSRunner::PumpPlatformMessages(bool forever)
{
	CocoaTools::RunCocoaEventLoop(forever);
}

void GSRunner::StopPlatformMessagePump()
{
	CocoaTools::StopMainThreadEventLoop();
}

#elif defined(__linux__)
static Display* s_display = nullptr;
static Window s_window = None;
static WindowInfo s_wi;
static std::atomic<bool> s_shutdown_requested{false};

bool GSRunner::CreatePlatformWindow()
{
	pxAssertRel(!s_display && s_window == None, "Tried to create window when there already was one!");

	s_display = XOpenDisplay(nullptr);
	if (!s_display)
	{
		Console.Error("Failed to open X11 display");
		return false;
	}

	int screen = DefaultScreen(s_display);
	Window root = RootWindow(s_display, screen);

	s_window = XCreateSimpleWindow(s_display, root, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, 1,
		BlackPixel(s_display, screen), WhitePixel(s_display, screen));

	if (s_window == None)
	{
		Console.Error("Failed to create X11 window");
		XCloseDisplay(s_display);
		s_display = nullptr;
		return false;
	}

	XStoreName(s_display, s_window, "PCSX2 GS Runner");
	XSelectInput(s_display, s_window, StructureNotifyMask);
	XMapWindow(s_display, s_window);

	s_wi.type = WindowInfo::Type::X11;
	s_wi.display_connection = s_display;
	s_wi.window_handle = reinterpret_cast<void*>(s_window);
	s_wi.surface_width = WINDOW_WIDTH;
	s_wi.surface_height = WINDOW_HEIGHT;
	s_wi.surface_scale = 1.0f;

	XFlush(s_display);
	PumpPlatformMessages();
	return true;
}

void GSRunner::DestroyPlatformWindow()
{
	if (s_display && s_window != None)
	{
		XDestroyWindow(s_display, s_window);
		s_window = None;
	}

	if (s_display)
	{
		XCloseDisplay(s_display);
		s_display = nullptr;
	}
}

std::optional<WindowInfo> GSRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;
	if (s_display && s_window != None)
		wi = s_wi;
	else
		wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void GSRunner::PumpPlatformMessages(bool forever)
{
	if (!s_display)
		return;

	do
	{
		while (XPending(s_display) > 0)
		{
			XEvent event;
			XNextEvent(s_display, &event);

			switch (event.type)
			{
				case ConfigureNotify:
				{
					const XConfigureEvent& configure = event.xconfigure;
					s_wi.surface_width = static_cast<u32>(configure.width);
					s_wi.surface_height = static_cast<u32>(configure.height);
					break;
				}
				case DestroyNotify:
					return;
				default:
					break;
			}
		}

		if (s_shutdown_requested.load())
			return;

		if (forever)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	} while (forever && !s_shutdown_requested.load());
}

void GSRunner::StopPlatformMessagePump()
{
	s_shutdown_requested.store(true);
}
#endif // _WIN32 / __APPLE__
