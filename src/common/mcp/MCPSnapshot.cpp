/*
** MCPSnapshot.cpp
**
** Game-thread publisher for the local MCP server snapshot.
** Read-only: copies counters into plain data, never mutates game state.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
*/

#include "common/mcp/MCPSnapshot.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "stats.h"
#include "hw_clock.h"
#include "i_time.h"
#include "dobjgc.h"
#include "doomstat.h"
#include "dthinker.h"
#include "actor.h"
#include "g_levellocals.h"
#include "zstring.h"

// Live off-switch (d_main.cpp). Checked every sample: setting
// mcp_gpu_stats 0 in console stops timer-query arming immediately.
EXTERN_CVAR(Bool, mcp_gpu_stats);

// Globals owned elsewhere (all read-only here).
extern cycle_t VMCycles[10];
extern int VMCalls[10];
extern cycle_t ACSTime;
extern cycle_t BotThinkCycles;
extern cycle_t BotSupportCycles;
extern int BotWTG;
extern cycle_t ActionCycles;
extern cycle_t FrameCycles; // D_Display duration (d_main.cpp), always clocked
extern uint64_t LastFPS;
extern uint64_t LastMSCount;
void CalcFps(); // d_main.cpp: updates LastFPS/LastMSCount (same as on-screen stat)

// Owned by the renderer (hw_postprocess.cpp); both backends fill
// gpuStatOutput with "name=xx.xx ms\n" lines when keepGpuStatActive
// arms one timer-query frame. gpuStatLatched holds the last non-empty
// sample so ~10Hz out-of-band readers don't race the per-frame transient.
// Write-only here (arm), read-only parse.
extern bool keepGpuStatActive;
extern FString gpuStatLatched;

namespace MCP
{

namespace
{
	std::mutex g_mutex;
	MCPFrameSnapshot g_snapshot;
	uint64_t g_lastFastMs = 0;
	uint64_t g_lastActorMs = 0;
	uint64_t g_lastProfileMs = 0;
	uint64_t g_profileCounter = 0;
	std::atomic<int> g_profileIntervalMs{ 1000 };
	std::atomic<int> g_actorIntervalMs{ 1000 };
	std::atomic<bool> g_actorRefresh{ false };
	std::atomic<bool> g_profileRequested{ false };
	std::atomic<bool> g_renderTimers{ false };
	std::atomic<bool> g_gpuStats{ false };
	uint64_t g_lastGpuReqMs = 0;
	FString g_lastGpuOutput; // last parsed sample; detects fresh query results

	constexpr int kMaxStoredClasses = 50;
	constexpr int kMaxStoredProfile = 100;
	constexpr uint64_t kGpuSampleIntervalMs = 1000;
} // namespace

void MCPSnapshot::SetProfileIntervalMs(int ms)
{
	if (ms < 500) ms = 500;
	if (ms > 60000) ms = 60000;
	g_profileIntervalMs.store(ms, std::memory_order_relaxed);
}

int MCPSnapshot::GetProfileIntervalMs()
{
	return g_profileIntervalMs.load(std::memory_order_relaxed);
}

void MCPSnapshot::SetActorIntervalMs(int ms)
{
	if (ms < 250) ms = 250;
	if (ms > 60000) ms = 60000;
	g_actorIntervalMs.store(ms, std::memory_order_relaxed);
}

void MCPSnapshot::SetRenderTimers(bool on)
{
	g_renderTimers.store(on, std::memory_order_relaxed);
}

bool MCPSnapshot::RenderTimersRequested()
{
	return g_renderTimers.load(std::memory_order_relaxed);
}

void MCPSnapshot::SetGpuStats(bool on)
{
	g_gpuStats.store(on, std::memory_order_relaxed);
}

bool MCPSnapshot::GpuStatsRequested()
{
	return g_gpuStats.load(std::memory_order_relaxed);
}

bool MCPSnapshot::RequestActorRefresh()
{
	g_actorRefresh.store(true, std::memory_order_relaxed);
	return true;
}

bool MCPSnapshot::RequestProfile()
{
	uint64_t now = I_msTime();
	uint64_t nextAllowed;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		nextAllowed = g_snapshot.profile_next_allowed_ms;
	}
	if (now < nextAllowed)
		return false; // cooldown: caller must serve cache
	g_profileRequested.store(true, std::memory_order_relaxed);
	return true;
}

bool MCPSnapshot::ConsumeProfileRequest()
{
	bool expected = true;
	if (!g_profileRequested.compare_exchange_strong(expected, false, std::memory_order_acq_rel))
		return false;
	uint64_t now = I_msTime();
	if (now < g_lastProfileMs + (uint64_t)GetProfileIntervalMs())
		return false; // too soon: skip this tick, re-arm via next RequestProfile
	return true;
}

void MCPSnapshot::PublishProfile(std::vector<MCPProfileEntry> entries, int totalThinkers, uint64_t tick)
{
	uint64_t now = I_msTime();
	if (entries.size() > (size_t)kMaxStoredProfile)
		entries.resize(kMaxStoredProfile);
	std::lock_guard<std::mutex> lock(g_mutex);
	g_snapshot.profile = std::move(entries);
	g_snapshot.profile_truncated = g_snapshot.profile.size() >= (size_t)kMaxStoredProfile;
	g_snapshot.profile_ts_ms = now;
	g_snapshot.profile_tick = tick != 0 ? tick : ++g_profileCounter;
	if (g_snapshot.profile_tick == 0) g_snapshot.profile_tick = ++g_profileCounter;
	g_snapshot.profile_fresh = true;
	g_lastProfileMs = now;
	g_snapshot.profile_next_allowed_ms = now + (uint64_t)GetProfileIntervalMs();
	(void)totalThinkers;
}

MCPFrameSnapshot MCPSnapshot::GetCopy()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_snapshot; // copy
}

void MCPSnapshot::CaptureGpu(MCPFrameSnapshot &s, uint64_t now)
{
	if (!GpuStatsRequested() || !mcp_gpu_stats)
		return;
	// Steady gameplay only: never arm queries during load/wipe/startup
	// frames, where blocking query-result reads can stall on frames whose
	// queries never complete (black screen until menu/console). The 'stat
	// gpu' path is only ever armed mid-game for the same reason.
	// ~3s grace after level start for shader precompile/wipe to settle.
	if (gamestate != GS_LEVEL || primaryLevel == nullptr || primaryLevel->maptime < 105)
		return;
	// Arm one timer-query frame per second. The renderer collects the
	// results into gpuStatOutput 1-2 frames later.
	if (now >= g_lastGpuReqMs + kGpuSampleIntervalMs)
	{
		g_lastGpuReqMs = now;
		keepGpuStatActive = true;
	}
	// Parse only fresh samples: the latch advances only when the renderer
	// collects a new timer-query frame, so a changed string is a new sample.
	// NOTE: same-thread assumption — CaptureGpu runs on the game thread
	// (D_DoomLoop), as do FGLDebug::Update / UpdateGpuStats.
	if (gpuStatLatched.IsEmpty() || g_lastGpuOutput.Compare(gpuStatLatched) == 0)
		return;
	g_lastGpuOutput = gpuStatLatched;
	const char *p = gpuStatLatched.GetChars();
	int count = 0;
	while (p && *p && count < MCPFrameSnapshot::MaxGpuPasses)
	{
		const char *eq = strchr(p, '=');
		if (!eq || eq == p)
			break;
		size_t namelen = (size_t)(eq - p);
		if (namelen >= sizeof(s.gpu_passes[0].name))
			namelen = sizeof(s.gpu_passes[0].name) - 1;
		memcpy(s.gpu_passes[count].name, p, namelen);
		s.gpu_passes[count].name[namelen] = '\0';
		s.gpu_passes[count].ms = strtod(eq + 1, nullptr);
		count++;
		const char *nl = strchr(eq + 1, '\n');
		if (!nl)
		{
			p = nullptr; // last line has no trailing newline: fully parsed
			break;
		}
		p = nl + 1;
	}
	s.gpu_pass_count = count;
	s.gpu_truncated = (p != nullptr && strchr(p, '=') != nullptr);
	s.gpu_ts_ms = now;
}

void MCPSnapshot::CaptureFast(MCPFrameSnapshot &s, uint64_t now)
{
	s.ts_ms = now;
	CalcFps(); // same updater as the on-screen 'stat fps' counter (cheap)
	s.fps = LastFPS;
	s.frame_ms = LastMSCount;
	s.think_ms = MCPStats::ThinkMs();
	s.think_count = MCPStats::ThinkCount();
	s.action_ms = MCPStats::ActionMs();
	s.display_ms = FrameCycles.TimeMS();

	s.render_all_ms = RenderAll.TimeMS();
	s.render_total_ms = All.TimeMS();
	s.process_ms = ProcessAll.TimeMS();
	s.bsp_ms = Bsp.TimeMS();
	s.portal_ms = PortalAll.TimeMS();
	s.drawcalls_ms = drawcalls.TimeMS();
	s.postprocess_ms = PostProcess.TimeMS();
	s.finish_ms = Finish.TimeMS();
	s.rendered_sprites = rendered_sprites;
	s.rendered_lines = rendered_lines;
	s.rendered_flats = rendered_flats;
	s.rendered_decals = rendered_decals;
	s.rendered_portals = rendered_portals;
	s.vertexcount = vertexcount;

	double vmSum = 0.0, vmPeak = 0.0;
	int vmCalls = 0;
	for (int i = 0; i < 10; i++)
	{
		double t = VMCycles[i].TimeMS();
		vmSum += t;
		if (t > vmPeak) vmPeak = t;
		vmCalls += VMCalls[i];
	}
	s.vm_last10_ms = vmSum;
	s.vm_peak_ms = vmPeak;
	s.vm_calls = vmCalls;
	s.acs_ms = ACSTime.TimeMS();

	s.gc_alloc_kb = (GC::AllocBytes + 1023) >> 10;
	s.gc_threshold_kb = (GC::Threshold == (size_t)~(size_t)0 - 2) ? 0 : ((GC::Threshold + 1023) >> 10);
	s.gc_ms = MCPStats::GCMs();

	s.sight_ms = MCPStats::SightMs();
	s.sight_max_ms = MCPStats::SightMaxMs();
	s.sight_tests = MCPStats::SightTests();
	s.bot_think_ms = BotThinkCycles.TimeMS();
	s.bot_support_ms = BotSupportCycles.TimeMS();
	s.bot_wtg = BotWTG;

	CaptureGpu(s, now);

	if (gamestate == GS_LEVEL && primaryLevel != nullptr)
	{
		FLevelLocals *Level = primaryLevel;
		s.map_name = Level->MapName.GetChars();
		s.level_name = Level->LevelName.GetChars();
		s.sectors = (int)Level->sectors.Size();
		s.lines = (int)Level->lines.Size();
		s.total_monsters = Level->total_monsters;
		s.killed_monsters = Level->killed_monsters;
		s.total_items = Level->total_items;
		s.total_secrets = Level->total_secrets;
		s.level_time = Level->maptime;
	}
}

void MCPSnapshot::CaptureActors(MCPFrameSnapshot &s, uint64_t now)
{
	if (gamestate != GS_LEVEL || primaryLevel == nullptr)
	{
		s.total_actors = 0;
		s.total_thinkers = 0;
		s.top_classes.clear();
		s.actors_truncated = false;
		s.actor_ts_ms = now;
		return;
	}
	FLevelLocals *Level = primaryLevel;
	std::unordered_map<std::string, int> counts;
	counts.reserve(256);
	int totalThinkers = 0;
	int totalActors = 0;

	TThinkerIterator<DThinker> it(Level);
	DThinker *th;
	while ((th = it.Next()) != nullptr)
	{
		totalThinkers++;
		if (th->IsKindOf(NAME_Actor))
		{
			totalActors++;
			const char *cls = th->GetClass()->TypeName.GetChars();
			counts[cls ? cls : "?"]++;
		}
	}

	std::vector<MCPClassCount> sorted;
	sorted.reserve(counts.size());
	for (auto &kv : counts)
		sorted.push_back({ kv.first, kv.second });
	std::sort(sorted.begin(), sorted.end(), [](const MCPClassCount &a, const MCPClassCount &b)
	{
		return a.count > b.count;
	});
	bool truncated = sorted.size() > (size_t)kMaxStoredClasses;
	if (truncated)
		sorted.resize(kMaxStoredClasses);

	s.total_actors = totalActors;
	s.total_thinkers = totalThinkers;
	s.top_classes = std::move(sorted);
	s.actors_truncated = truncated;
	s.actor_ts_ms = now;
}

void MCPSnapshot::CaptureIfDue()
{
	uint64_t now = I_msTime();
	int actorInterval = g_actorIntervalMs.load(std::memory_order_relaxed);
	bool needActor = g_actorRefresh.exchange(false, std::memory_order_acq_rel);

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (now - g_lastFastMs < 100 && !needActor)
			return; // fast path: nothing due
		CaptureFast(g_snapshot, now);
		g_lastFastMs = now;
		if (needActor || now - g_lastActorMs >= (uint64_t)actorInterval || g_snapshot.actor_ts_ms == 0)
		{
			// CaptureActors walks thinkers; do it while holding the lock so
			// GetCopy() never sees a torn census. Walk is O(N), ~1s TTL.
			CaptureActors(g_snapshot, now);
			g_lastActorMs = now;
		}
		if (g_snapshot.profile_next_allowed_ms == 0)
			g_snapshot.profile_next_allowed_ms = now;
	}
}

} // namespace MCP
