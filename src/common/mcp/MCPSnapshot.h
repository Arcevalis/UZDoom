/*
** MCPSnapshot.h
**
** Read-only snapshot of engine performance state for the local MCP server.
** Game thread publishes; MCP worker threads consume copies only.
** Never touches GC-managed memory off the game thread.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
*/

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Read-only accessors for statics owned by other translation units.
// Defined next to the statics they read; they never mutate state.
namespace MCPStats
{
	double ThinkMs();
	int ThinkCount();
	double ActionMs();
	double SightMs();
	double SightMaxMs();
	int SightTests();
	double GCMs();
} // namespace MCPStats

namespace MCP
{

// One GPU timer-query pass, parsed from gpuStatOutput ("name=xx.xx ms" lines).
struct MCPGpuPass
{
	char name[48] = {};
	double ms = 0.0;
};

struct MCPClassCount
{
	std::string name;
	int count = 0;
};

struct MCPProfileEntry
{
	std::string name;
	int calls = 0;
	double total_ms = 0.0;
	double avg_ms = 0.0;
};

// Flat copy of everything an MCP tool may return. Plain data only so the
// HTTP thread can serialize without touching engine memory.
struct MCPFrameSnapshot
{
	uint64_t ts_ms = 0;

	// Frame / think totals (cheap, refreshed ~10Hz)
	uint64_t fps = 0;
	uint64_t frame_ms = 0;
	double think_ms = 0.0;
	int think_count = 0;
	double action_ms = 0.0;
	double display_ms = 0.0; // D_Display duration incl. render+present (FrameCycles)

	// Render (externs in hw_clock.h, cheap reads)
	double render_all_ms = 0.0;
	double render_total_ms = 0.0;
	double process_ms = 0.0;
	double bsp_ms = 0.0;
	double portal_ms = 0.0;
	double drawcalls_ms = 0.0;
	double postprocess_ms = 0.0;
	double finish_ms = 0.0; // swap-buffer/present cost
	int rendered_sprites = 0;
	int rendered_lines = 0;
	int rendered_flats = 0;
	int rendered_decals = 0;
	int rendered_portals = 0;
	int vertexcount = 0;

	// VM / ACS (cheap ring-buffer reads)
	double vm_last10_ms = 0.0;
	double vm_peak_ms = 0.0;
	int vm_calls = 0;
	double acs_ms = 0.0;

	// GC (extern counters + GCMs accessor)
	size_t gc_alloc_kb = 0;
	size_t gc_threshold_kb = 0;
	double gc_ms = 0.0;

	// Sight / bots (accessors + externs)
	double sight_ms = 0.0;
	double sight_max_ms = 0.0;
	int sight_tests = 0;
	double bot_think_ms = 0.0;
	double bot_support_ms = 0.0;
	int bot_wtg = 0;

	// Level (copied strings + counts)
	std::string map_name;
	std::string level_name;
	int sectors = 0;
	int lines = 0;
	int total_monsters = 0;
	int killed_monsters = 0;
	int total_items = 0;
	int total_secrets = 0;
	int level_time = 0;

	// Actor census (O(N) walk, TTL-gated, default 1s)
	uint64_t actor_ts_ms = 0;
	int total_actors = 0;
	int total_thinkers = 0;
	std::vector<MCPClassCount> top_classes;
	bool actors_truncated = false;

	// Thinker timing profile (instrumented tick, min-interval gated)
	uint64_t profile_ts_ms = 0;
	uint64_t profile_tick = 0;
	bool profile_fresh = false;
	uint64_t profile_next_allowed_ms = 0;
	std::vector<MCPProfileEntry> profile;
	bool profile_truncated = false;

	// GPU pass timings (timer queries, sampled ~1Hz while server runs)
	static constexpr int MaxGpuPasses = 24;
	uint64_t gpu_ts_ms = 0;
	int gpu_pass_count = 0;
	bool gpu_truncated = false;
	MCPGpuPass gpu_passes[MaxGpuPasses] = {};
};

class MCPSnapshot
{
public:
	// Called once on the game thread per frame; internally TTL-gated.
	static void CaptureIfDue();

	// Force a fresh actor census / profile tick on next opportunity.
	// Server thread calls Request* (non-blocking); game thread honors them.
	static bool RequestActorRefresh();
	static bool RequestProfile();

	// Game-thread side: consume pending requests (called from RunThinkers /
	// CaptureIfDue). Returns true if a profiled tick should run now.
	static bool ConsumeProfileRequest();

	// Game-thread side: publish results of an instrumented tick.
	static void PublishProfile(std::vector<MCPProfileEntry> entries, int totalThinkers, uint64_t tick);

	// Server-thread side: copy-on-read. Never touches engine memory.
	static MCPFrameSnapshot GetCopy();

	static void SetProfileIntervalMs(int ms);
	static int GetProfileIntervalMs();

	static void SetActorIntervalMs(int ms);

	// Render-timer request: while true, the renderer keeps glcycle_t
	// profiling active (checked per frame). Profiling toggle only;
	// never affects game state, determinism, or rendering.
	static void SetRenderTimers(bool on);
	static bool RenderTimersRequested();

	// GPU-stat sampling: while true, CaptureFast arms one timer-query
	// frame per second via keepGpuStatActive and parses gpuStatOutput.
	// Same profiling-toggle-only guarantee as render timers.
	static void SetGpuStats(bool on);
	static bool GpuStatsRequested();

private:
	static void CaptureFast(MCPFrameSnapshot &s, uint64_t now);
	static void CaptureActors(MCPFrameSnapshot &s, uint64_t now);
	static void CaptureGpu(MCPFrameSnapshot &s, uint64_t now);
};

} // namespace MCP
