/*
** MCPServer.cpp
**
** Localhost Streamable-HTTP MCP server, stateless + read-only.
** Serves POST /mcp (JSON-RPC), GET /health. Binds 127.0.0.1 only.
** Worker threads serialize MCPSnapshot copies; never touch engine memory.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
*/

#include "common/mcp/MCPServer.h"
#include "common/mcp/MCPSnapshot.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using mcp_socket_t = SOCKET;
static const mcp_socket_t MCP_INVALID_SOCKET = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using mcp_socket_t = int;
static const mcp_socket_t MCP_INVALID_SOCKET = -1;
#endif

using nlohmann::json;

namespace MCP
{
namespace
{
constexpr size_t kMaxHeaderBytes = 65536;
constexpr size_t kMaxBodyBytes = 4 * 1024 * 1024;
constexpr const char *kServerVersion = "5.0.0-pre";
constexpr const char *kProtocolVersion = "2024-11-05";

void CloseSocket(mcp_socket_t fd)
{
	if (fd == MCP_INVALID_SOCKET) return;
#ifdef _WIN32
	closesocket(fd);
#else
	::close(fd);
#endif
}

std::string ToLower(std::string s)
{
	for (char &c : s) c = (char)tolower((unsigned char)c);
	return s;
}

std::string Trim(const std::string &s)
{
	size_t a = 0;
	while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) a++;
	size_t b = s.size();
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
	return s.substr(a, b - a);
}

bool SendAll(mcp_socket_t fd, const char *data, size_t len)
{
	size_t sent = 0;
	while (sent < len)
	{
#ifdef _WIN32
		int n = ::send(fd, data + sent, (int)(len - sent), 0);
#else
		ssize_t n = ::send(fd, data + sent, len - sent, 0);
#endif
		if (n <= 0) return false;
		sent += (size_t)n;
	}
	return true;
}

bool OriginAllowed(const std::string &origin)
{
	if (origin.empty()) return true;
	std::string lo = ToLower(origin);
	if (lo.find("localhost") != std::string::npos) return true;
	if (lo.find("127.0.0.1") != std::string::npos) return true;
	if (lo == "null") return true;
	// Non-browser MCP clients (OpenCode, curl, inspector) usually omit Origin.
	// Only reject an explicit foreign http(s) origin (DNS-rebinding defense).
	if (lo.rfind("http://", 0) == 0 || lo.rfind("https://", 0) == 0) return false;
	return true;
}

// ---- Snapshot -> JSON (pure, no engine access) ----

json ProfileEntriesJson(const std::vector<MCPProfileEntry> &v, int limit, bool &truncated)
{
	truncated = (int)v.size() > limit;
	json arr = json::array();
	for (int i = 0; i < (int)v.size() && i < limit; i++)
	{
		arr.push_back({
			{ "class", v[(size_t)i].name },
			{ "calls", v[(size_t)i].calls },
			{ "total_ms", v[(size_t)i].total_ms },
			{ "avg_ms", v[(size_t)i].avg_ms },
		});
	}
	return arr;
}

json ClassesJson(const std::vector<MCPClassCount> &v, int limit, bool &truncated)
{
	truncated = (int)v.size() > limit;
	json arr = json::array();
	for (int i = 0; i < (int)v.size() && i < limit; i++)
		arr.push_back({ { "class", v[(size_t)i].name }, { "count", v[(size_t)i].count } });
	return arr;
}

json PerfJson(const MCPFrameSnapshot &s)
{
	return {
		{ "ts_ms", s.ts_ms },
		{ "fps", s.fps },
		{ "frame_ms", s.frame_ms },
		{ "think_ms", s.think_ms },
		{ "think_count", s.think_count },
		{ "action_ms", s.action_ms },
		{ "display_ms", s.display_ms },
		{ "render_all_ms", s.render_all_ms },
		{ "render_total_ms", s.render_total_ms },
		{ "process_ms", s.process_ms },
		{ "bsp_ms", s.bsp_ms },
		{ "vm_last10_ms", s.vm_last10_ms },
		{ "vm_peak_ms", s.vm_peak_ms },
		{ "vm_calls", s.vm_calls },
		{ "acs_ms", s.acs_ms },
		{ "gc_alloc_kb", s.gc_alloc_kb },
		{ "gc_threshold_kb", s.gc_threshold_kb },
		{ "gc_ms", s.gc_ms },
		{ "sight_ms", s.sight_ms },
		{ "sight_max_ms", s.sight_max_ms },
		{ "sight_tests", s.sight_tests },
		{ "bot_think_ms", s.bot_think_ms },
		{ "bot_support_ms", s.bot_support_ms },
		{ "bot_wtg", s.bot_wtg },
	};
}

json RenderJson(const MCPFrameSnapshot &s)
{
	json gpu = json::array();
	int gpuCount = s.gpu_pass_count < MCPFrameSnapshot::MaxGpuPasses ? s.gpu_pass_count : MCPFrameSnapshot::MaxGpuPasses;
	if (gpuCount < 0) gpuCount = 0;
	for (int i = 0; i < gpuCount; i++)
		gpu.push_back({ { "pass", std::string(s.gpu_passes[i].name) }, { "ms", s.gpu_passes[i].ms } });
	// Fresh if a timer-query sample landed within the last 5s
	// (the sampler arms one query frame per second while the server runs).
	bool gpuFresh = gpuCount > 0 && s.ts_ms >= s.gpu_ts_ms && (s.ts_ms - s.gpu_ts_ms) < 5000;
	return {
		{ "ts_ms", s.ts_ms },
		{ "rendered_sprites", s.rendered_sprites },
		{ "rendered_lines", s.rendered_lines },
		{ "rendered_flats", s.rendered_flats },
		{ "rendered_decals", s.rendered_decals },
		{ "rendered_portals", s.rendered_portals },
		{ "vertexcount", s.vertexcount },
		{ "render_all_ms", s.render_all_ms },
		{ "render_total_ms", s.render_total_ms },
		{ "process_ms", s.process_ms },
		{ "bsp_ms", s.bsp_ms },
		{ "portal_ms", s.portal_ms },
		{ "drawcalls_ms", s.drawcalls_ms },
		{ "postprocess_ms", s.postprocess_ms },
		{ "finish_ms", s.finish_ms },
		{ "gpu_ts_ms", s.gpu_ts_ms },
		{ "gpu_fresh", gpuFresh },
		{ "gpu_truncated", s.gpu_truncated },
		{ "gpu_passes", gpu },
	};
}

json LevelJson(const MCPFrameSnapshot &s)
{
	return {
		{ "map", s.map_name },
		{ "level_name", s.level_name },
		{ "sectors", s.sectors },
		{ "lines", s.lines },
		{ "total_monsters", s.total_monsters },
		{ "killed_monsters", s.killed_monsters },
		{ "total_items", s.total_items },
		{ "total_secrets", s.total_secrets },
		{ "level_time", s.level_time },
	};
}

json SnapshotJson(const MCPFrameSnapshot &s)
{
	json j = PerfJson(s);
	j["render"] = RenderJson(s);
	j["level"] = LevelJson(s);
	j["vm_acs"] = { { "vm_last10_ms", s.vm_last10_ms }, { "vm_peak_ms", s.vm_peak_ms }, { "vm_calls", s.vm_calls }, { "acs_ms", s.acs_ms } };
	j["actors"] = { { "total_actors", s.total_actors }, { "total_thinkers", s.total_thinkers }, { "actor_ts_ms", s.actor_ts_ms }, { "truncated", s.actors_truncated } };
	bool t = false;
	j["actors"]["top_classes"] = ClassesJson(s.top_classes, 50, t);
	j["profile"] = { { "profile_ts_ms", s.profile_ts_ms }, { "profile_tick", s.profile_tick }, { "fresh", s.profile_fresh }, { "next_allowed_ms", s.profile_next_allowed_ms }, { "truncated", s.profile_truncated } };
	j["profile"]["entries"] = ProfileEntriesJson(s.profile, 100, t);
	return j;
}

// ---- JSON-RPC helpers ----

json RpcError(const json &id, int code, const std::string &msg)
{
	json e = { { "jsonrpc", "2.0" }, { "error", { { "code", code }, { "message", msg } } } };
	if (!id.is_null()) e["id"] = id;
	else e["id"] = nullptr;
	return e;
}

int IntArg(const json &args, const char *key, int def, int lo, int hi)
{
	try
	{
		if (args.is_object() && args.contains(key) && args[key].is_number())
		{
			int v = args[key].get<int>();
			if (v < lo) v = lo;
			if (v > hi) v = hi;
			return v;
		}
	}
	catch (...) {}
	return def;
}

bool BoolArg(const json &args, const char *key, bool def)
{
	try
	{
		if (args.is_object() && args.contains(key) && args[key].is_boolean())
			return args[key].get<bool>();
	}
	catch (...) {}
	return def;
}

MCPFrameSnapshot WaitForActorRefresh(uint64_t prevTs, int timeoutMs)
{
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	MCPSnapshot::RequestActorRefresh();
	MCPFrameSnapshot s = MCPSnapshot::GetCopy();
	while (s.actor_ts_ms == prevTs && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
		s = MCPSnapshot::GetCopy();
	}
	return s;
}

MCPFrameSnapshot WaitForProfile(uint64_t prevTs, int timeoutMs)
{
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	MCPFrameSnapshot s = MCPSnapshot::GetCopy();
	while (s.profile_ts_ms == prevTs && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		s = MCPSnapshot::GetCopy();
	}
	return s;
}

json ToolResult(const json &payload)
{
	std::string text = payload.dump();
	return { { "content", json::array({ { { "type", "text" }, { "text", text } } }) } };
}

json DispatchTool(const std::string &name, const json &args)
{
	if (name == "get_snapshot")
	{
		return ToolResult(SnapshotJson(MCPSnapshot::GetCopy()));
	}
	if (name == "get_performance_stats")
	{
		return ToolResult(PerfJson(MCPSnapshot::GetCopy()));
	}
	if (name == "get_render_stats")
	{
		return ToolResult(RenderJson(MCPSnapshot::GetCopy()));
	}
	if (name == "get_level_info")
	{
		return ToolResult(LevelJson(MCPSnapshot::GetCopy()));
	}
	if (name == "get_vm_acs_stats")
	{
		MCPFrameSnapshot s = MCPSnapshot::GetCopy();
		return ToolResult(json{ { "vm_last10_ms", s.vm_last10_ms }, { "vm_peak_ms", s.vm_peak_ms }, { "vm_calls", s.vm_calls }, { "acs_ms", s.acs_ms }, { "ts_ms", s.ts_ms } });
	}
	if (name == "get_actor_stats")
	{
		int top = IntArg(args, "top", 20, 1, 50);
		bool refresh = BoolArg(args, "refresh", false);
		MCPFrameSnapshot s = MCPSnapshot::GetCopy();
		bool waited = false;
		if (refresh)
		{
			s = WaitForActorRefresh(s.actor_ts_ms, 600);
			waited = true;
		}
		bool trunc = false;
		json arr = ClassesJson(s.top_classes, top, trunc);
		(void)waited;
		return ToolResult(json{
			{ "total_actors", s.total_actors },
			{ "total_thinkers", s.total_thinkers },
			{ "actor_ts_ms", s.actor_ts_ms },
			{ "top_classes", arr },
			{ "truncated", trunc || s.actors_truncated },
		});
	}
	if (name == "profile_thinkers")
	{
		int limit = IntArg(args, "limit", 20, 1, 100);
		bool refresh = BoolArg(args, "refresh", false);
		MCPFrameSnapshot s = MCPSnapshot::GetCopy();
		bool fresh = s.profile_fresh;
		uint64_t retryAfter = 0;
		if (s.profile_next_allowed_ms > s.ts_ms)
			retryAfter = s.profile_next_allowed_ms - s.ts_ms;
		if (refresh)
		{
			if (MCPSnapshot::RequestProfile())
			{
				// Armed for the next instrumented tick; wait briefly.
				// Only claim fresh if a new tick actually arrived: on
				// timeout the request stays armed for a later tick, so
				// report the cache as stale instead of misreporting it.
				uint64_t prevTs = s.profile_ts_ms;
				s = WaitForProfile(prevTs, 1500);
				if (s.profile_ts_ms != prevTs)
				{
					fresh = true;
					retryAfter = 0;
				}
				else
				{
					fresh = false;
					retryAfter = (uint64_t)MCPSnapshot::GetProfileIntervalMs();
				}
			}
			else
			{
				fresh = false; // cooldown: serve cache
			}
			if (s.profile_next_allowed_ms > s.ts_ms)
				retryAfter = s.profile_next_allowed_ms - s.ts_ms;
		}
		bool trunc = false;
		json arr = ProfileEntriesJson(s.profile, limit, trunc);
		return ToolResult(json{
			{ "profile_ts_ms", s.profile_ts_ms },
			{ "profile_tick", s.profile_tick },
			{ "fresh", fresh },
			{ "retry_after_ms", retryAfter },
			{ "entries", arr },
			{ "truncated", trunc || s.profile_truncated },
		});
	}
	return json{ { "isError", true }, { "content", json::array({ { { "type", "text" }, { "text", "unknown tool: " + name } } }) } };
}

json ToolsList()
{
	auto schemaTop = json{ { "type", "object" }, { "properties", json{ { "top", json{ { "type", "integer" }, { "minimum", 1 }, { "maximum", 50 }, { "default", 20 } } }, { "refresh", json{ { "type", "boolean" }, { "default", false } } } } } };
	auto schemaLimit = json{ { "type", "object" }, { "properties", json{ { "limit", json{ { "type", "integer" }, { "minimum", 1 }, { "maximum", 100 }, { "default", 20 } } }, { "refresh", json{ { "type", "boolean" }, { "default", false } } } } } };
	auto schemaEmpty = json{ { "type", "object" }, { "properties", json::object() } };
	json tools = json::array();
	tools.push_back({ { "name", "get_snapshot" }, { "description", "Full read-only performance snapshot (frame, think, render, VM/ACS, GC, level, actors, profile)." }, { "inputSchema", schemaEmpty } });
	tools.push_back({ { "name", "get_performance_stats" }, { "description", "Frame ms, FPS, think/action*time, render totals, VM, ACS, GC, sight, bots." }, { "inputSchema", schemaEmpty } });
	tools.push_back({ { "name", "get_actor_stats" }, { "description", "Actor/thinker census with per-class counts. Set refresh:true to recapture (TTL ~1s)." }, { "inputSchema", schemaTop } });
	tools.push_back({ { "name", "profile_thinkers" }, { "description", "Per-class thinker timing from an instrumented tick. Min interval 1s; returns cached entry with fresh:false + retry_after_ms during cooldown." }, { "inputSchema", schemaLimit } });
	tools.push_back({ { "name", "get_render_stats" }, { "description", "Rendered sprites/lines/flats/decals/portals, vertex counts, CPU render pass timings, GPU timer-query pass breakdown (sampled ~1Hz)." }, { "inputSchema", schemaEmpty } });
	tools.push_back({ { "name", "get_level_info" }, { "description", "Current map name, sector/line counts, monster/item/secret totals." }, { "inputSchema", schemaEmpty } });
	tools.push_back({ { "name", "get_vm_acs_stats" }, { "description", "ZScript VM 10-tic window and ACS time." }, { "inputSchema", schemaEmpty } });
	return tools;
}

} // namespace

MCPServer::MCPServer() = default;
MCPServer::~MCPServer() { Stop(); }

bool MCPServer::Listen(int port)
{
	std::lock_guard<std::mutex> lock(stateMutex);
	if (running.load(std::memory_order_acquire)) return boundPort == port;
	if (port <= 0 || port > 65535) return false;

#ifdef _WIN32
	WSADATA ws;
	if (WSAStartup(MAKEWORD(2, 2), &ws) != 0) return false;
#endif

	mcp_socket_t fd =
#ifdef _WIN32
		::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
		::socket(AF_INET, SOCK_STREAM, 0);
#endif
	if (fd == MCP_INVALID_SOCKET)
	{
#ifdef _WIN32
		WSACleanup();
#endif
		return false;
	}

	int one = 1;
#ifdef _WIN32
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
#else
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only
	if (::bind(fd, (sockaddr *)&addr, sizeof(addr)) != 0)
	{
		CloseSocket(fd);
#ifdef _WIN32
		WSACleanup();
#endif
		return false;
	}
	if (::listen(fd, 8) != 0)
	{
		CloseSocket(fd);
#ifdef _WIN32
		WSACleanup();
#endif
		return false;
	}

	listenFd = fd;
	boundPort = port;
	stopRequested.store(false, std::memory_order_release);
	running.store(true, std::memory_order_release);
	// Profiling toggle only (see MCPSnapshot::SetRenderTimers): keeps the
	// renderer's glcycle_t timers clocked so render timings read non-zero.
	// No effect on game state, determinism, or rendering.
	MCPSnapshot::SetRenderTimers(true);
	// Same guarantee for GPU timer queries: arms one query frame per
	// second while the server runs (see MCPSnapshot::SetGpuStats).
	MCPSnapshot::SetGpuStats(true);
	listener = std::thread(&MCPServer::ListenerLoop, this);
	return true;
}

void MCPServer::Stop()
{
	bool was = running.exchange(false, std::memory_order_acq_rel);
	stopRequested.store(true, std::memory_order_release);
	if (listenFd != MCP_INVALID_SOCKET)
	{
#ifdef _WIN32
		::shutdown(listenFd, SD_BOTH);
#else
		::shutdown(listenFd, SHUT_RDWR);
#endif
		CloseSocket(listenFd);
		listenFd = MCP_INVALID_SOCKET;
	}
	if (listener.joinable())
		listener.join();
#ifdef _WIN32
	if (was) WSACleanup();
#else
	(void)was;
#endif
	if (was)
		MCPSnapshot::SetRenderTimers(false);
	if (was)
		MCPSnapshot::SetGpuStats(false);
	boundPort = 0;
}

void MCPServer::ListenerLoop()
{
	while (!stopRequested.load(std::memory_order_acquire))
	{
		fd_set rfds;
		FD_ZERO(&rfds);
		if (listenFd != MCP_INVALID_SOCKET)
			FD_SET(listenFd, &rfds);
		else
			break;
		timeval tv{ 0, 100000 }; // 100ms so Stop() joins promptly
		int rc;
#ifdef _WIN32
		rc = ::select(0, &rfds, nullptr, nullptr, &tv);
#else
		rc = ::select(listenFd + 1, &rfds, nullptr, nullptr, &tv);
#endif
		if (rc <= 0) continue;
		sockaddr_in cli{};
		socklen_t len = sizeof(cli);
		mcp_socket_t c =
#ifdef _WIN32
			::accept(listenFd, (sockaddr *)&cli, &len);
#else
			::accept(listenFd, (sockaddr *)&cli, &len);
#endif
		if (c == MCP_INVALID_SOCKET) continue;
		// Only loopback peers (defense in depth; socket is bound to 127.0.0.1).
		uint32_t peer = ntohl(cli.sin_addr.s_addr);
		if (peer != INADDR_LOOPBACK)
		{
			CloseSocket(c);
			continue;
		}
		int one = 1;
#ifdef _WIN32
		::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
#else
		::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
		std::thread(&MCPServer::HandleClient, this, (int)(intptr_t)c).detach();
	}
}

static bool RecvAll(mcp_socket_t fd, std::string &out, size_t want, size_t cap)
{
	while (out.size() < want)
	{
		if (out.size() > cap) return false;
		char buf[8192];
		size_t need = want - out.size();
		if (need > sizeof(buf)) need = sizeof(buf);
#ifdef _WIN32
		int n = ::recv(fd, buf, (int)need, 0);
#else
		ssize_t n = ::recv(fd, buf, need, 0);
#endif
		if (n <= 0) return false;
		out.append(buf, (size_t)n);
	}
	return true;
}

void MCPServer::HandleClient(int fdRaw)
{
	mcp_socket_t fd = (mcp_socket_t)(intptr_t)fdRaw;
	std::string data;
	// Read headers.
	while (data.find("\r\n\r\n") == std::string::npos)
	{
		if (data.size() > kMaxHeaderBytes) { CloseSocket(fd); return; }
		char buf[4096];
#ifdef _WIN32
		int n = ::recv(fd, buf, sizeof(buf), 0);
#else
		ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
#endif
		if (n <= 0) { CloseSocket(fd); return; }
		data.append(buf, (size_t)n);
	}
	size_t hlen = data.find("\r\n\r\n") + 4;
	std::string head = data.substr(0, hlen);
	std::istringstream hs(head);
	std::string requestLine;
	std::getline(hs, requestLine);
	if (!requestLine.empty() && requestLine.back() == '\r') requestLine.pop_back();
	std::istringstream rl(requestLine);
	std::string method, target, version;
	rl >> method >> target >> version;
	method = ToLower(method) == "post" ? "POST" : (ToLower(method) == "get" ? "GET" : (ToLower(method) == "options" ? "OPTIONS" : method));

	std::map<std::string, std::string> headers;
	std::string line;
	while (std::getline(hs, line) && line != "\r" && !line.empty())
	{
		if (!line.empty() && line.back() == '\r') line.pop_back();
		size_t c = line.find(':');
		if (c == std::string::npos) continue;
		headers[ToLower(Trim(line.substr(0, c)))] = Trim(line.substr(c + 1));
	}

	if (!OriginAllowed(headers.count("origin") ? headers["origin"] : ""))
	{
		std::string body = "{\"error\":\"forbidden origin\"}";
		std::string resp = "HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
		SendAll(fd, resp.data(), resp.size());
		CloseSocket(fd);
		return;
	}

	if (method == "OPTIONS")
	{
		std::string resp = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: POST, GET, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type, Accept, MCP-Protocol-Version, Mcp-Session-Id\r\nConnection: close\r\n\r\n";
		SendAll(fd, resp.data(), resp.size());
		CloseSocket(fd);
		return;
	}

	if (method == "GET" && (target == "/health" || target == "/health/"))
	{
		json h = { { "status", "ok" }, { "server", "uzdoom-mcp" }, { "version", kServerVersion } };
		std::string body = h.dump();
		std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
		SendAll(fd, resp.data(), resp.size());
		CloseSocket(fd);
		return;
	}

	if (target != "/mcp" && target != "/mcp/")
	{
		std::string body = "{\"error\":\"not found\"}";
		std::string resp = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
		SendAll(fd, resp.data(), resp.size());
		CloseSocket(fd);
		return;
	}

	if (method == "GET")
	{
		// No SSE stream in stateless mode; inform client to use POST.
		std::string body = "{\"error\":\"use POST /mcp for JSON-RPC\"}";
		std::string resp = "HTTP/1.1 405 Method Not Allowed\r\nContent-Type: application/json\r\nAllow: POST\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
		SendAll(fd, resp.data(), resp.size());
		CloseSocket(fd);
		return;
	}

	if (method != "POST")
	{
		std::string body = "{\"error\":\"method not allowed\"}";
		std::string resp = "HTTP/1.1 405 Method Not Allowed\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
		SendAll(fd, resp.data(), resp.size());
		CloseSocket(fd);
		return;
	}

	size_t contentLength = 0;
	auto it = headers.find("content-length");
	if (it != headers.end())
	{
		try { contentLength = (size_t)std::stoul(it->second); } catch (...) { contentLength = 0; }
	}
	if (contentLength > kMaxBodyBytes)
	{
		std::string body = "{\"error\":\"payload too large\"}";
		std::string resp = "HTTP/1.1 413 Payload Too Large\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
		SendAll(fd, resp.data(), resp.size());
		CloseSocket(fd);
		return;
	}
	std::string raw = data.substr(hlen);
	if (!RecvAll(fd, raw, contentLength, kMaxBodyBytes)) { CloseSocket(fd); return; }

	json req;
	try { req = json::parse(raw); }
	catch (...)
	{
		json e = RpcError(json(nullptr), -32700, "parse error");
		std::string body = e.dump();
		std::string resp = "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
		SendAll(fd, resp.data(), resp.size());
		CloseSocket(fd);
		return;
	}

	bool isNotification = !req.contains("id") || req["id"].is_null();
	json id = req.contains("id") ? req["id"] : json(nullptr);
	std::string rpcMethod = req.value("method", "");
	json params = req.contains("params") ? req["params"] : json::object();

	if (rpcMethod == "notifications/initialized")
	{
		std::string resp = "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
		SendAll(fd, resp.data(), resp.size());
		CloseSocket(fd);
		return;
	}

	json result;
	int httpStatus = 200;
	std::string httpText = "OK";
	if (rpcMethod == "initialize")
	{
		std::string pv = kProtocolVersion;
		try
		{
			if (params.is_object() && params.contains("protocolVersion") && params["protocolVersion"].is_string())
				pv = params["protocolVersion"].get<std::string>();
		}
		catch (...) {}
		result = { { "protocolVersion", pv }, { "capabilities", { { "tools", json::object() }, { "resources", json::object() } } }, { "serverInfo", { { "name", "uzdoom-mcp" }, { "version", kServerVersion } } } };
	}
	else if (rpcMethod == "ping")
	{
		result = json::object();
	}
	else if (rpcMethod == "tools/list")
	{
		result = { { "tools", ToolsList() } };
	}
	else if (rpcMethod == "resources/list")
	{
		result = { { "resources", json::array() } };
	}
	else if (rpcMethod == "tools/call")
	{
		std::string tname;
		json targs = json::object();
		try
		{
			if (params.is_object())
			{
				if (params.contains("name") && params["name"].is_string()) tname = params["name"].get<std::string>();
				if (params.contains("arguments")) targs = params["arguments"];
			}
		}
		catch (...) {}
		if (tname.empty())
		{
			json e = RpcError(id, -32602, "missing tool name");
			std::string body = e.dump();
			std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
			SendAll(fd, resp.data(), resp.size());
			CloseSocket(fd);
			return;
		}
		result = DispatchTool(tname, targs);
	}
	else
	{
		json e = RpcError(id, -32601, "method not found: " + rpcMethod);
		std::string body = e.dump();
		std::string resp = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
		SendAll(fd, resp.data(), resp.size());
		CloseSocket(fd);
		return;
	}

	if (isNotification)
	{
		std::string resp = "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
		SendAll(fd, resp.data(), resp.size());
		CloseSocket(fd);
		return;
	}

	json envelope = { { "jsonrpc", "2.0" }, { "id", id }, { "result", result } };
	std::string body = envelope.dump();
	std::string resp = "HTTP/1.1 " + std::to_string(httpStatus) + " " + httpText + "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
	SendAll(fd, resp.data(), resp.size());
	CloseSocket(fd);
}

} // namespace MCP
