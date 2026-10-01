#include "../../stdafx.hpp"

#include <bcrypt.h>
#include <winhttp.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include "../bingo.hpp"

namespace Bingo
{
	namespace Platform
	{
		namespace
		{
			// Board input comes from subclassing the game window's window procedure
			// (plain Win32, so it also works under Wine/Proton)
			// The cursor position comes from WM_MOUSEMOVE, which stays correct with RInput
			HWND game_window = nullptr;
			WNDPROC original_wndproc = nullptr;
			bool captured = false;

			HWND FindGameWindow()
			{
				// GoldSrc's main window class
				HWND hwnd = nullptr;
				while ((hwnd = FindWindowExA(nullptr, hwnd, "Valve001", nullptr)) != nullptr) {
					DWORD pid;
					GetWindowThreadProcessId(hwnd, &pid);
					if (pid == GetCurrentProcessId())
						return hwnd;
				}

				return nullptr;
			}

			LRESULT CALLBACK BingoWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
			{
				// Stays installed once it is, and only acts while the board has the input
				if (!captured)
					return CallWindowProcA(original_wndproc, hwnd, msg, wParam, lParam);

				if (msg >= WM_LBUTTONDOWN && msg <= WM_MOUSELAST)
					Input::DebugPrint("mouse message 0x%x, wParam 0x%x\n", msg, static_cast<unsigned>(wParam));

				switch (msg) {
				case WM_ACTIVATEAPP:
					if (!wParam)
						Input::OnFocusLost();
					break;

				// Alt+F4: close the board first so the crosshair is restored before the config is saved
				case WM_CLOSE:
					Input::OnFocusLost();
					break;

				case WM_MOUSEMOVE: {
					RECT rect;
					if (GetClientRect(hwnd, &rect) && rect.right > 0 && rect.bottom > 0) {
						int x = static_cast<short>(LOWORD(lParam));
						int y = static_cast<short>(HIWORD(lParam));
						Input::OnMouseMove(static_cast<float>(x) / rect.right, static_cast<float>(y) / rect.bottom);
					}
					return 0;
				}

				case WM_LBUTTONDOWN:
				case WM_LBUTTONDBLCLK:
					Input::OnMouseButton("MOUSE1", true);
					return 0;
				case WM_LBUTTONUP:
					Input::OnMouseButton("MOUSE1", false);
					return 0;
				case WM_RBUTTONDOWN:
				case WM_RBUTTONDBLCLK:
					Input::OnMouseButton("MOUSE2", true);
					return 0;
				case WM_RBUTTONUP:
					Input::OnMouseButton("MOUSE2", false);
					return 0;
				case WM_MBUTTONDOWN:
				case WM_MBUTTONDBLCLK:
					Input::OnMouseButton("MOUSE3", true);
					return 0;
				case WM_MBUTTONUP:
					Input::OnMouseButton("MOUSE3", false);
					return 0;
				case WM_XBUTTONDOWN:
				case WM_XBUTTONDBLCLK:
					Input::OnMouseButton(GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? "MOUSE4" : "MOUSE5", true);
					return TRUE;
				case WM_XBUTTONUP:
					Input::OnMouseButton(GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? "MOUSE4" : "MOUSE5", false);
					return TRUE;
				case WM_MOUSEWHEEL: {
					auto name = GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? "MWHEELUP" : "MWHEELDOWN";
					Input::OnMouseButton(name, true);
					Input::OnMouseButton(name, false);
					return 0;
				}

				// Only the navigation keys are taken, other keys reach the game
				case WM_KEYDOWN:
					Input::DebugPrint("WM_KEYDOWN %u\n", static_cast<unsigned>(wParam));
					switch (wParam) {
					case VK_UP:
						Input::OnKey(Input::Key::UP);
						return 0;
					case VK_DOWN:
						Input::OnKey(Input::Key::DOWN);
						return 0;
					case VK_LEFT:
						Input::OnKey(Input::Key::LEFT);
						return 0;
					case VK_RIGHT:
						Input::OnKey(Input::Key::RIGHT);
						return 0;
					case VK_RETURN:
						Input::OnKey(Input::Key::ENTER);
						return 0;
					case VK_ESCAPE:
						Input::OnKey(Input::Key::ESCAPE);
						return 0;
					}
					break;

				case WM_KEYUP:
					switch (wParam) {
					case VK_UP:
					case VK_DOWN:
					case VK_LEFT:
					case VK_RIGHT:
					case VK_RETURN:
					case VK_ESCAPE:
						return 0;
					}
					break;
				}

				return CallWindowProcA(original_wndproc, hwnd, msg, wParam, lParam);
			}
		}

		bool IsSupported()
		{
			return true;
		}

		std::string Sha256File(const std::string& path, uint64_t& size)
		{
			size = 0;

			std::ifstream file(path, std::ios::binary);
			if (!file)
				return {};

			BCRYPT_ALG_HANDLE alg = nullptr;
			if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
				return {};

			std::string result;
			BCRYPT_HASH_HANDLE hash = nullptr;
			if (BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
				std::vector<char> buffer(64 * 1024);
				bool ok = true;
				while (ok && file) {
					file.read(buffer.data(), buffer.size());
					auto count = file.gcount();
					if (count > 0) {
						size += count;
						ok = BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0));
					}
				}

				unsigned char digest[32];
				if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
					char hex[sizeof(digest) * 2 + 1];
					for (size_t i = 0; i < sizeof(digest); ++i)
						sprintf(hex + i * 2, "%02x", digest[i]);
					result = hex;
				}

				BCryptDestroyHash(hash);
			}

			BCryptCloseAlgorithmProvider(alg, 0);
			return result;
		}

		bool CaptureInput()
		{
			if (captured)
				return true;

			HWND hwnd = FindGameWindow();
			if (!hwnd)
				return false;

			// Subclass again if the engine created a new window (e.g. after a video mode change)
			if (hwnd != game_window) {
				auto previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(BingoWndProc)));
				if (!previous)
					return false;

				game_window = hwnd;
				original_wndproc = previous;
			}

			// The board draws its own cursor, so keep the hidden one inside the window
			RECT rect;
			if (GetClientRect(hwnd, &rect)) {
				POINT top_left = { rect.left, rect.top };
				POINT bottom_right = { rect.right, rect.bottom };
				ClientToScreen(hwnd, &top_left);
				ClientToScreen(hwnd, &bottom_right);

				RECT clip = { top_left.x, top_left.y, bottom_right.x, bottom_right.y };
				ClipCursor(&clip);
			}

			captured = true;
			return true;
		}

		void ReleaseInput()
		{
			if (!captured)
				return;

			captured = false;
			ClipCursor(nullptr);
		}

		namespace
		{
			// Time limits for finding the server, connecting and sending
			constexpr int CONNECT_TIMEOUT_MS = 10000;

			// A connection with nothing coming in for this long is dead...
			// The server answers a ping every 30s, so a live one never gets there
			constexpr int RECEIVE_TIMEOUT_MS = 75000;

			// The biggest message taken from the server, the manifest is the largest
			constexpr size_t MAX_MESSAGE_SIZE = 4 * 1024 * 1024;

			// How much of a refusal's body is kept, it's a short JSON reason
			constexpr size_t MAX_REFUSAL_SIZE = 4096;

			std::wstring Widen(const std::string& text)
			{
				if (text.empty())
					return {};

				int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
				std::wstring result(size, L'\0');
				MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &result[0], size);
				return result;
			}

			// A part of an address from WinHttpCrackUrl, which leaves it null when the address has none
			std::wstring UrlPart(const wchar_t* text, DWORD length)
			{
				return text ? std::wstring(text, length) : std::wstring();
			}

			// What the game thread and the workers share
			// Kept alive by whoever still uses it, so the game can drop a socket at any time
			struct SocketState
			{
				std::mutex mutex;
				std::condition_variable wake_sender;
				std::deque<SocketEvent> events;
				std::deque<std::string> outgoing;
				bool close_requested = false;
				USHORT close_code = WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS;
				bool stop_sender = false;

				void Push(SocketEvent::Type type, int code, std::string text)
				{
					SocketEvent event;
					event.type = type;
					event.code = code;
					event.text = std::move(text);

					std::lock_guard<std::mutex> lock(mutex);
					events.push_back(std::move(event));
				}
			};

			// Sends the queued messages in order, then the close frame when asked
			// Runs next to the receiving worker, as WinHTTP allows one send and one receive at a time
			void SendLoop(std::shared_ptr<SocketState> state, HINTERNET websocket)
			{
				for (;;) {
					std::string text;
					USHORT close_code = 0;
					{
						std::unique_lock<std::mutex> lock(state->mutex);
						state->wake_sender.wait(lock, [&] {
							return state->stop_sender || state->close_requested || !state->outgoing.empty();
						});

						if (state->stop_sender)
							return;

						if (!state->outgoing.empty()) {
							text = std::move(state->outgoing.front());
							state->outgoing.pop_front();
						} else
							close_code = state->close_code;
					}

					DWORD error;
					if (close_code) {
						// The receiver gets the server's answer and ends the connection
						error = WinHttpWebSocketShutdown(websocket, close_code, nullptr, 0);
					} else
						error = WinHttpWebSocketSend(websocket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &text[0], static_cast<DWORD>(text.size()));

					// After closing, or when the connection is gone (the receiver sees that too), only wait to be stopped
					if (close_code || error != NO_ERROR) {
						std::unique_lock<std::mutex> lock(state->mutex);
						state->wake_sender.wait(lock, [&] { return state->stop_sender; });
						return;
					}
				}
			}

			// Connects, then receives until the connection ends. Runs on its own thread
			void ReceiveLoop(std::shared_ptr<SocketState> state, std::string url, std::vector<std::string> headers)
			{
				using Type = SocketEvent::Type;

				HINTERNET session = nullptr, connection = nullptr, request = nullptr, websocket = nullptr;
				std::thread sender;

				[&] {
					// WinHttpCrackUrl only knows http(s), and WebSockets use the same default ports
					bool secure;
					if (url.compare(0, 6, "wss://") == 0) {
						secure = true;
						url = "https://" + url.substr(6);
					} else if (url.compare(0, 5, "ws://") == 0) {
						secure = false;
						url = "http://" + url.substr(5);
					} else {
						state->Push(Type::FAILED, 0, "the address must start with ws:// or wss://");
						return;
					}

					auto wide_url = Widen(url);
					URL_COMPONENTS parts = {};
					parts.dwStructSize = sizeof(parts);
					parts.dwHostNameLength = static_cast<DWORD>(-1);
					parts.dwUrlPathLength = static_cast<DWORD>(-1);
					parts.dwExtraInfoLength = static_cast<DWORD>(-1);
					if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) {
						state->Push(Type::FAILED, 0, "can't read the address (error " + std::to_string(GetLastError()) + ")");
						return;
					}

					auto host = UrlPart(parts.lpszHostName, parts.dwHostNameLength);
					auto path = UrlPart(parts.lpszUrlPath, parts.dwUrlPathLength) + UrlPart(parts.lpszExtraInfo, parts.dwExtraInfoLength);
					if (path.empty())
						path = L"/";

					session = WinHttpOpen(L"BunnymodXT bingo", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
					if (!session) {
						state->Push(Type::FAILED, 0, "WinHttpOpen failed (error " + std::to_string(GetLastError()) + ")");
						return;
					}

					WinHttpSetTimeouts(session, CONNECT_TIMEOUT_MS, CONNECT_TIMEOUT_MS, CONNECT_TIMEOUT_MS, RECEIVE_TIMEOUT_MS);

					connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
					if (!connection) {
						state->Push(Type::FAILED, 0, "WinHttpConnect failed (error " + std::to_string(GetLastError()) + ")");
						return;
					}

					request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
					if (!request) {
						state->Push(Type::FAILED, 0, "WinHttpOpenRequest failed (error " + std::to_string(GetLastError()) + ")");
						return;
					}

					// The documented way to ask for the upgrade is no buffer, which the SDK's annotation doesn't allow for
#pragma warning(suppress : 6387)
					if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
						state->Push(Type::FAILED, 0, "WebSockets aren't available (error " + std::to_string(GetLastError()) + ", they need Windows 8 or newer)");
						return;
					}

					std::wstring header_text;
					for (const auto& header : headers)
						header_text += Widen(header) + L"\r\n";

					if (!WinHttpSendRequest(request, header_text.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_text.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
						|| !WinHttpReceiveResponse(request, nullptr)) {
						state->Push(Type::FAILED, 0, "can't reach the server (error " + std::to_string(GetLastError()) + ")");
						return;
					}

					DWORD status = 0, status_size = sizeof(status);
					WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
					if (status != 101) {
						// A refusal says why in its body, e.g. {"error":"bad_code"}
						std::string body;
						DWORD available = 0;
						while (body.size() < MAX_REFUSAL_SIZE && WinHttpQueryDataAvailable(request, &available) && available > 0) {
							std::vector<char> buffer(std::min<size_t>(available, MAX_REFUSAL_SIZE - body.size()));
							DWORD read = 0;
							if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read) || read == 0)
								break;
							body.append(buffer.data(), read);
						}

						state->Push(Type::REFUSED, static_cast<int>(status), std::move(body));
						return;
					}

					websocket = WinHttpWebSocketCompleteUpgrade(request, 0);
					if (!websocket) {
						state->Push(Type::FAILED, 0, "the WebSocket upgrade failed (error " + std::to_string(GetLastError()) + ")");
						return;
					}

					WinHttpCloseHandle(request);
					request = nullptr;

					state->Push(Type::OPENED, 0, {});
					sender = std::thread(SendLoop, state, websocket);

					std::string message;
					std::vector<char> buffer(16 * 1024);
					for (;;) {
						DWORD read = 0;
						WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
						DWORD error = WinHttpWebSocketReceive(websocket, buffer.data(), static_cast<DWORD>(buffer.size()), &read, &type);
						if (error != NO_ERROR) {
							state->Push(Type::CLOSED, 1006, error == ERROR_WINHTTP_TIMEOUT ? "the server stopped answering" : "the connection dropped (error " + std::to_string(error) + ")");
							return;
						}

						if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
							USHORT code = 0;
							char reason[WINHTTP_WEB_SOCKET_MAX_CLOSE_REASON_LENGTH] = {};
							DWORD reason_size = 0;
							WinHttpWebSocketQueryCloseStatus(websocket, &code, reason, sizeof(reason), &reason_size);
							state->Push(Type::CLOSED, code, std::string(reason, reason_size));
							return;
						}

						message.append(buffer.data(), read);
						if (message.size() > MAX_MESSAGE_SIZE) {
							state->Push(Type::CLOSED, 1009, "the server sent a message that's too big");
							return;
						}

						if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
							continue;

						// The protocol only has text messages
						if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
							state->Push(Type::MESSAGE, 0, std::move(message));

						message.clear();
					}
				}();

				// The sender stops before the handles it uses go away
				if (sender.joinable()) {
					{
						std::lock_guard<std::mutex> lock(state->mutex);
						state->stop_sender = true;
					}
					state->wake_sender.notify_all();
					sender.join();
				}

				if (websocket) {
					WinHttpWebSocketClose(websocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
					WinHttpCloseHandle(websocket);
				}
				if (request)
					WinHttpCloseHandle(request);
				if (connection)
					WinHttpCloseHandle(connection);
				if (session)
					WinHttpCloseHandle(session);
			}

			class WinHttpSocket : public Socket
			{
			public:
				explicit WinHttpSocket(std::shared_ptr<SocketState> state)
					: state(std::move(state))
				{
				}

				~WinHttpSocket() override
				{
					Close(WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS);
				}

				void Send(const std::string& text) override
				{
					{
						std::lock_guard<std::mutex> lock(state->mutex);
						if (state->close_requested)
							return;
						state->outgoing.push_back(text);
					}
					state->wake_sender.notify_all();
				}

				void Close(int code) override
				{
					{
						std::lock_guard<std::mutex> lock(state->mutex);
						if (state->close_requested)
							return;
						state->close_requested = true;
						state->close_code = static_cast<USHORT>(code);
					}
					state->wake_sender.notify_all();
				}

				bool Poll(SocketEvent& event) override
				{
					std::lock_guard<std::mutex> lock(state->mutex);
					if (state->events.empty())
						return false;

					event = std::move(state->events.front());
					state->events.pop_front();
					return true;
				}

			private:
				std::shared_ptr<SocketState> state;
			};
		}

		std::unique_ptr<Socket> OpenSocket(const std::string& url, const std::vector<std::string>& headers)
		{
			auto state = std::make_shared<SocketState>();
			std::thread(ReceiveLoop, state, url, headers).detach();
			return std::make_unique<WinHttpSocket>(state);
		}

		namespace
		{
			// Time limits for a download: finding the server and connecting, then each read
			constexpr int DOWNLOAD_CONNECT_TIMEOUT_MS = 10000;
			constexpr int DOWNLOAD_RECEIVE_TIMEOUT_MS = 30000;

			// Each file is downloaded at most this many times when it keeps failing its check
			constexpr int DOWNLOAD_TRIES = 2;

			// What the game thread and the worker share
			struct SyncState
			{
				std::mutex mutex;
				std::deque<FileEvent> events;
				std::atomic<bool> stopped{ false };

				void Push(FileEvent::Type type, size_t index, std::string text = {})
				{
					FileEvent event;
					event.type = type;
					event.index = index;
					event.text = std::move(text);

					std::lock_guard<std::mutex> lock(mutex);
					events.push_back(std::move(event));
				}
			};

			// The file's size, or false if it isn't there
			bool FileSize(const std::string& path, uint64_t& size)
			{
				WIN32_FILE_ATTRIBUTE_DATA data;
				if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
					return false;

				size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
				return true;
			}

			// Makes the folders a file goes in, e.g. valve/sound/bingo for valve/sound/bingo/firework.wav
			void CreateFolders(const std::string& path)
			{
				for (size_t i = path.find_first_of("/\\"); i != std::string::npos; i = path.find_first_of("/\\", i + 1))
					CreateDirectoryA(path.substr(0, i).c_str(), nullptr);
			}

			// Downloads url into path, at most size bytes. Returns why it failed, or an empty string
			std::string Fetch(HINTERNET session, const std::string& url, const std::string& path, uint64_t size, const std::atomic<bool>& stopped)
			{
				auto wide_url = Widen(url);
				URL_COMPONENTS parts = {};
				parts.dwStructSize = sizeof(parts);
				parts.dwSchemeLength = static_cast<DWORD>(-1);
				parts.dwHostNameLength = static_cast<DWORD>(-1);
				parts.dwUrlPathLength = static_cast<DWORD>(-1);
				parts.dwExtraInfoLength = static_cast<DWORD>(-1);
				if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts))
					return "can't read the address " + url;

				auto host = UrlPart(parts.lpszHostName, parts.dwHostNameLength);
				auto request_path = UrlPart(parts.lpszUrlPath, parts.dwUrlPathLength) + UrlPart(parts.lpszExtraInfo, parts.dwExtraInfoLength);

				std::string error;
				HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
				HINTERNET request = connection
					? WinHttpOpenRequest(connection, L"GET", request_path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
					: nullptr;

				[&] {
					if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
						|| !WinHttpReceiveResponse(request, nullptr)) {
						error = "can't reach the server (error " + std::to_string(GetLastError()) + ")";
						return;
					}

					DWORD status = 0, status_size = sizeof(status);
					WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
					if (status != 200) {
						error = status == 404 ? "the server doesn't have it" : "the server answered " + std::to_string(status);
						return;
					}

					std::ofstream out(path, std::ios::binary | std::ios::trunc);
					if (!out) {
						error = "can't write " + path;
						return;
					}

					uint64_t total = 0;
					std::vector<char> buffer(64 * 1024);
					for (;;) {
						if (stopped) {
							error = "stopped";
							return;
						}

						DWORD read = 0;
						if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
							error = "the download broke off (error " + std::to_string(GetLastError()) + ")";
							return;
						}
						if (read == 0)
							break;

						total += read;
						if (total > size) {
							error = "it's bigger than it should be";
							return;
						}

						if (!out.write(buffer.data(), read)) {
							error = "can't write " + path;
							return;
						}
					}

					if (!out.flush())
						error = "can't write " + path;
					else if (total != size)
						error = "it's smaller than it should be";
				}();

				if (request)
					WinHttpCloseHandle(request);
				if (connection)
					WinHttpCloseHandle(connection);
				return error;
			}

			// Goes through the files in order. Runs on its own thread
			void SyncLoop(std::shared_ptr<SyncState> state, std::vector<FileJob> jobs)
			{
				using Type = FileEvent::Type;

				// Each sync writes its own .part files, so one that's being stopped can't get in the way
				static std::atomic<unsigned> sync_count{ 0 };
				auto part_suffix = "." + std::to_string(++sync_count) + ".part";

				HINTERNET session = nullptr;
				for (size_t i = 0; i < jobs.size() && !state->stopped; ++i) {
					const auto& job = jobs[i];

					uint64_t size = 0;
					bool exists = FileSize(job.path, size);
					if (exists && size == job.size) {
						uint64_t hashed;
						if (Sha256File(job.path, hashed) == job.sha256 && hashed == job.size) {
							state->Push(Type::PRESENT, i);
							continue;
						}
					}

					if (exists && !job.replace) {
						state->Push(Type::CONFLICT, i);
						continue;
					}

					if (!session) {
						session = WinHttpOpen(L"BunnymodXT bingo", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
						if (session)
							WinHttpSetTimeouts(session, DOWNLOAD_CONNECT_TIMEOUT_MS, DOWNLOAD_CONNECT_TIMEOUT_MS, DOWNLOAD_CONNECT_TIMEOUT_MS, DOWNLOAD_RECEIVE_TIMEOUT_MS);
					}
					if (!session) {
						state->Push(Type::FAILED, i, "WinHttpOpen failed (error " + std::to_string(GetLastError()) + ")");
						continue;
					}

					CreateFolders(job.path);
					auto part = job.path + part_suffix;
					std::string error;
					for (int tries = 0; tries < DOWNLOAD_TRIES && !state->stopped; ++tries) {
						error = Fetch(session, job.url, part, job.size, state->stopped);
						if (error.empty()) {
							uint64_t hashed;
							if (Sha256File(part, hashed) != job.sha256 || hashed != job.size)
								error = "what the server sent doesn't match the file's SHA-256";
						}
						if (error.empty())
							break;
					}

					if (error.empty() && !state->stopped && !MoveFileExA(part.c_str(), job.path.c_str(), MOVEFILE_REPLACE_EXISTING))
						error = "can't replace " + job.path + " (error " + std::to_string(GetLastError()) + ")";

					DeleteFileA(part.c_str());
					if (state->stopped)
						break;

					if (error.empty())
						state->Push(Type::DOWNLOADED, i);
					else
						state->Push(Type::FAILED, i, std::move(error));
				}

				if (session)
					WinHttpCloseHandle(session);
			}

			class WinFileSync : public FileSync
			{
			public:
				explicit WinFileSync(std::shared_ptr<SyncState> state)
					: state(std::move(state))
				{
				}

				~WinFileSync() override
				{
					state->stopped = true;
				}

				bool Poll(FileEvent& event) override
				{
					std::lock_guard<std::mutex> lock(state->mutex);
					if (state->events.empty())
						return false;

					event = std::move(state->events.front());
					state->events.pop_front();
					return true;
				}

			private:
				std::shared_ptr<SyncState> state;
			};
		}

		std::unique_ptr<FileSync> SyncFiles(std::vector<FileJob> jobs)
		{
			auto state = std::make_shared<SyncState>();
			std::thread(SyncLoop, state, std::move(jobs)).detach();
			return std::make_unique<WinFileSync>(state);
		}

		namespace
		{
			// Time limits for an upload
			constexpr int UPLOAD_CONNECT_TIMEOUT_MS = 10000;
			constexpr int UPLOAD_SEND_TIMEOUT_MS = 60000;

			// What the game thread and the worker share
			struct UploadState
			{
				std::mutex mutex;
				std::deque<UploadEvent> events;
				std::atomic<bool> stopped{ false };

				void Push(bool ok, size_t index, std::string text = {})
				{
					UploadEvent event;
					event.ok = ok;
					event.index = index;
					event.text = std::move(text);

					std::lock_guard<std::mutex> lock(mutex);
					events.push_back(std::move(event));
				}
			};

			// Sends the file with a PUT. Returns why it failed, or an empty string
			std::string Put(HINTERNET session, const UploadJob& job, const std::atomic<bool>& stopped)
			{
				uint64_t size = 0;
				if (!FileSize(job.path, size))
					return "can't find " + job.path;
				if (size > 0xffffffffULL)
					return job.path + " is too big";

				std::ifstream in(job.path, std::ios::binary);
				if (!in)
					return "can't read " + job.path;

				auto wide_url = Widen(job.url);
				URL_COMPONENTS parts = {};
				parts.dwStructSize = sizeof(parts);
				parts.dwSchemeLength = static_cast<DWORD>(-1);
				parts.dwHostNameLength = static_cast<DWORD>(-1);
				parts.dwUrlPathLength = static_cast<DWORD>(-1);
				parts.dwExtraInfoLength = static_cast<DWORD>(-1);
				if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts))
					return "can't read the address " + job.url;

				auto host = UrlPart(parts.lpszHostName, parts.dwHostNameLength);
				auto request_path = UrlPart(parts.lpszUrlPath, parts.dwUrlPathLength) + UrlPart(parts.lpszExtraInfo, parts.dwExtraInfoLength);

				std::wstring headers;
				for (const auto& header : job.headers)
					headers += Widen(header) + L"\r\n";
				headers += L"Content-Type: application/octet-stream\r\n";

				std::string error;
				HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
				HINTERNET request = connection
					? WinHttpOpenRequest(connection, L"PUT", request_path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
					: nullptr;

				[&] {
					if (!request || !WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, static_cast<DWORD>(size), 0)) {
						error = "can't reach the server (error " + std::to_string(GetLastError()) + ")";
						return;
					}

					std::vector<char> buffer(64 * 1024);
					uint64_t sent = 0;
					while (sent < size) {
						if (stopped) {
							error = "stopped";
							return;
						}

						auto count = static_cast<DWORD>(std::min<uint64_t>(buffer.size(), size - sent));
						if (!in.read(buffer.data(), count)) {
							error = "can't read " + job.path;
							return;
						}

						DWORD written = 0;
						if (!WinHttpWriteData(request, buffer.data(), count, &written) || written != count) {
							error = "the upload broke off (error " + std::to_string(GetLastError()) + ")";
							return;
						}
						sent += count;
					}

					if (!WinHttpReceiveResponse(request, nullptr)) {
						error = "the server didn't answer (error " + std::to_string(GetLastError()) + ")";
						return;
					}

					DWORD status = 0, status_size = sizeof(status);
					WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
					if (status < 200 || status >= 300)
						error = "the server answered " + std::to_string(status);
				}();

				if (request)
					WinHttpCloseHandle(request);
				if (connection)
					WinHttpCloseHandle(connection);
				return error;
			}

			// Goes through the files in order. Runs on its own thread
			void UploadLoop(std::shared_ptr<UploadState> state, std::vector<UploadJob> jobs)
			{
				HINTERNET session = WinHttpOpen(L"BunnymodXT bingo", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
				if (!session) {
					state->Push(false, 0, "WinHttpOpen failed (error " + std::to_string(GetLastError()) + ")");
					return;
				}
				WinHttpSetTimeouts(session, UPLOAD_CONNECT_TIMEOUT_MS, UPLOAD_CONNECT_TIMEOUT_MS, UPLOAD_SEND_TIMEOUT_MS, UPLOAD_SEND_TIMEOUT_MS);

				for (size_t i = 0; i < jobs.size() && !state->stopped; ++i) {
					auto error = Put(session, jobs[i], state->stopped);
					if (state->stopped)
						break;

					bool ok = error.empty();
					state->Push(ok, i, std::move(error));
					if (!ok)
						break;
				}

				WinHttpCloseHandle(session);
			}

			class WinFileUpload : public FileUpload
			{
			public:
				explicit WinFileUpload(std::shared_ptr<UploadState> state)
					: state(std::move(state))
				{
				}

				~WinFileUpload() override
				{
					state->stopped = true;
				}

				bool Poll(UploadEvent& event) override
				{
					std::lock_guard<std::mutex> lock(state->mutex);
					if (state->events.empty())
						return false;

					event = std::move(state->events.front());
					state->events.pop_front();
					return true;
				}

			private:
				std::shared_ptr<UploadState> state;
			};
		}

		std::unique_ptr<FileUpload> UploadFiles(std::vector<UploadJob> jobs)
		{
			auto state = std::make_shared<UploadState>();
			std::thread(UploadLoop, state, std::move(jobs)).detach();
			return std::make_unique<WinFileUpload>(state);
		}

		std::string ModulePath()
		{
			// The module this function is in
			HMODULE module = nullptr;
			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(&ModulePath), &module))
				return {};

			char path[MAX_PATH];
			auto length = GetModuleFileNameA(module, path, MAX_PATH);
			return length > 0 && length < MAX_PATH ? std::string(path, length) : std::string();
		}
	}
}
