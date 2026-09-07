/*
** MCPServer.h
**
** Localhost Streamable-HTTP MCP server (stateless, read-only).
** Binds 127.0.0.1 only. Game thread publishes snapshots; worker threads
** serve copies. Never touches GC-managed memory off the game thread.
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
#include <mutex>
#include <string>
#include <thread>

namespace MCP
{

class MCPServer
{
public:
	MCPServer();
	~MCPServer();

	MCPServer(const MCPServer &) = delete;
	MCPServer &operator=(const MCPServer &) = delete;

	// Bind 127.0.0.1:port and start the listener thread. Returns false on error.
	bool Listen(int port);
	void Stop();
	bool IsRunning() const { return running.load(std::memory_order_acquire); }
	int GetPort() const { return boundPort; }

private:
	void ListenerLoop();
	void HandleClient(int fd);

	std::thread listener;
	std::atomic<bool> running{ false };
	std::atomic<bool> stopRequested{ false };
	int listenFd = -1;
	int boundPort = 0;
	std::mutex stateMutex;
};

} // namespace MCP
