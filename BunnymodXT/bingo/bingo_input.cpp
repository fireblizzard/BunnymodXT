#include "../stdafx.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>

#include "../modules/ClientDLL.hpp"
#include "../modules/HwDLL.hpp"
#include "../cvars.hpp"
#include "bingo_internal.hpp"

namespace Bingo
{
	namespace
	{
		// The board closes itself when it stops being drawn, so an invisible board can't keep the mouse
		// It takes both limits: a level transition skips ~20 frames (~300 ms) at 100 fps, more
		// frames at a higher fps_max, and a loading game briefly isn't in a map either
		// A disconnect is caught right away through the client state instead
		constexpr unsigned long long MAX_UNDRAWN_FRAMES = 60;
		constexpr long long MAX_UNDRAWN_MS = 1000;

		// Set while a mouse button is handed to the engine
		bool forwarding_key = false;

		xcommand_t original_toggleconsole = nullptr;
		xcommand_t original_showconsole = nullptr;

		// The engine draws its crosshair after the HUD, so it's hidden while the board is open
		void HideCrosshair()
		{
			auto engfuncs = ClientDLL::GetInstance().pEngfuncs;
			board.saved_crosshair = engfuncs->pfnGetCvarFloat(const_cast<char*>("crosshair"));
			if (board.saved_crosshair != 0)
				engfuncs->Cvar_SetValue(const_cast<char*>("crosshair"), 0);
		}

		void RestoreCrosshair()
		{
			if (board.saved_crosshair == 0)
				return;

			ClientDLL::GetInstance().pEngfuncs->Cvar_SetValue(const_cast<char*>("crosshair"), board.saved_crosshair);
			board.saved_crosshair = 0;
		}

		// Engine key number for a mouse button or wheel name, or -1
		int KeyNumber(const char* key_name)
		{
			static const std::pair<const char*, int> keys[] = {
				{ "MOUSE1", K_MOUSE1 },
				{ "MOUSE2", K_MOUSE2 },
				{ "MOUSE3", K_MOUSE3 },
				{ "MOUSE4", K_MOUSE4 },
				{ "MOUSE5", K_MOUSE5 },
				{ "MWHEELUP", K_MWHEELUP },
				{ "MWHEELDOWN", K_MWHEELDOWN },
			};

			for (const auto& key : keys) {
				if (std::strcmp(key.first, key_name) == 0)
					return key.second;
			}

			return -1;
		}

		// Key_LookupBinding only returns the first key bound to a command
		// Fallback for engine builds without Key_Event
		bool IsBoardBind(const char* key_name)
		{
			auto engfuncs = ClientDLL::GetInstance().pEngfuncs;
			if (!engfuncs || !engfuncs->Key_LookupBinding)
				return false;

			auto bound_key = engfuncs->Key_LookupBinding("bxt_bingo_board");
			return bound_key && boost::iequals(bound_key, key_name);
		}

		void OpenBoard()
		{
			if (!manifest.loaded) {
				Print("No board is loaded. Use bxt_bingo_manifest <file>.\n");
				return;
			}

			if (!IsInMap()) {
				Print("The board can only be opened in a map.\n");
				return;
			}

			auto& cl = ClientDLL::GetInstance();
			cl.SetMouseState(false);
			if (!Platform::CaptureInput()) {
				cl.SetMouseState(true);
				Print("The interactive board isn't supported on this engine build yet. The mini-board (bxt_hud_bingo) still works.\n");
				return;
			}

			board.open = true;
			board.mouse_seen = false;
			board.close_requested = false;
			board.pick_requested = false;
			board.pressed_button.clear();
			board.pressed_tile = -1;
			board.last_drawn_frame = frame_count;
			board.last_drawn_time = std::chrono::steady_clock::now();
			board.left_map_since_drawn = false;
			board.client_state = HwDLL::GetInstance().GetClientState();
			HideCrosshair();
			if (current_tile >= 0)
				board.selected = current_tile;
		}

		// On HL WON at least `Con_IsVisible()` is not working well, so we wrap this to
		// close the board and give the mouse back before the console opens
		void WrappedToggleconsole()
		{
			CloseBoard(true);
			original_toggleconsole();
		}

		void WrappedShowconsole()
		{
			CloseBoard(true);
			original_showconsole();
		}
	}

	Board board;

	// restore_mouse is false when the engine takes the mouse itself (console, menu, focus loss)
	void CloseBoard(bool restore_mouse)
	{
		if (!board.open)
			return;

		board.open = false;
		board.close_requested = false;
		board.pick_requested = false;

		RestoreCrosshair();
		Platform::ReleaseInput();
		if (restore_mouse)
			ClientDLL::GetInstance().SetMouseState(true);
	}

	// Called once from Init()
	void WrapConsoleCommands()
	{
		WrapCommand("toggleconsole", original_toggleconsole, WrappedToggleconsole);
		WrapCommand("showconsole", original_showconsole, WrappedShowconsole);
	}

	// The engine skips a key's bind when HUD_Key_Event returns 0
	// While the board hands a mouse button to the engine, only bxt_bingo_board may run,
	// so the button shouldn't do anything else in the game (like +attack2)
	// Keyboard events are not touched here
	bool AllowKeyEvent(const char* binding)
	{
		if (!forwarding_key)
			return true;

		std::string command = binding ? binding : "";
		boost::algorithm::trim(command);
		bool allowed = boost::iequals(command, "bxt_bingo_board");
		Input::DebugPrint("bind \"%s\": %s\n", binding ? binding : "", allowed ? "runs" : "blocked");
		return allowed;
	}

	// Called every frame from Frame()
	// Closes the board when it can't be seen, and picks the tile the player chose on it
	void BoardFrame()
	{
		if (!board.open)
			return;

		auto engfuncs = ClientDLL::GetInstance().pEngfuncs;
		if (engfuncs && engfuncs->Con_IsVisible && engfuncs->Con_IsVisible()) {
			CloseBoard(false);
			return;
		}

		// From cactive_t enum:  1 = disconnected (main menu), 5 = playing
		int client_state = HwDLL::GetInstance().GetClientState();
		if (client_state != board.client_state) {
			Input::DebugPrint("client state %d -> %d\n", board.client_state, client_state);
			board.client_state = client_state;
		}

		if (client_state == ca_disconnected) {
			Input::DebugPrint("closing the board, disconnected\n");
			CloseBoard(false);
			return;
		}

		auto undrawn_frames = frame_count - board.last_drawn_frame;
		auto undrawn_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - board.last_drawn_time).count();
		bool in_map = IsInMap();
		if (!in_map)
			board.left_map_since_drawn = true;

		// This is just a guess for unknown edge cases
		if (undrawn_frames > MAX_UNDRAWN_FRAMES && undrawn_ms > MAX_UNDRAWN_MS) {
			Input::DebugPrint("closing the board, it wasn't drawn for %llu frames (%lld ms), in a map: %d\n",
				undrawn_frames, static_cast<long long>(undrawn_ms), in_map ? 1 : 0);
			CloseBoard(false);
			return;
		}

		if (board.close_requested) {
			CloseBoard(true);
			return;
		}

		if (board.pick_requested) {
			board.pick_requested = false;
			PickTile(board.selected);
		}
	}

	void ToggleBoard()
	{
		if (board.open)
			CloseBoard(true);
		else
			OpenBoard();
	}

	namespace Input
	{
		void OnMouseMove(float x, float y)
		{
			board.mouse_seen = true;
			board.mouse_x = x;
			board.mouse_y = y;

			int hovered = HoveredTile();
			if (hovered >= 0)
				board.selected = hovered;
		}

		void DebugPrint(const char* format, ...)
		{
			if (!CVars::_bxt_bingo_debug_input.GetBool())
				return;

			char buffer[512];
			va_list args;
			va_start(args, format);
			vsnprintf(buffer, sizeof(buffer), format, args);
			va_end(args);

			Print("[bingo input] %s", buffer);
		}

		void OnMouseButton(const char* key_name, bool down)
		{
			if (CVars::_bxt_bingo_debug_input.GetBool()) {
				auto engfuncs = ClientDLL::GetInstance().pEngfuncs;
				auto bound_key = engfuncs && engfuncs->Key_LookupBinding ? engfuncs->Key_LookupBinding("bxt_bingo_board") : nullptr;
				DebugPrint("%s %s, board open: %d, bxt_bingo_board is bound to: %s\n",
					key_name, down ? "down" : "up", board.open ? 1 : 0, bound_key ? bound_key : "(nothing)");
			}

			if (!board.open)
				return;

			if (down) {
				board.pressed_button = key_name;
				board.pressed_tile = HoveredTile();
				return;
			}

			// A release without a press means the button went down before the board opened
			// (e.g. the mouse button bound to the board)
			// Hand the release to the engine, which missed it, or it would think the button is still held
			if (board.pressed_button != key_name) {
				int key = KeyNumber(key_name);
				if (key >= 0) {
					bool sent = HwDLL::GetInstance().SendKeyEvent(key, 0);
					DebugPrint("%s release without a press, handed to the engine: %s\n", key_name, sent ? "yes" : "no (Key_Event not found)");
				}
				return;
			}

			board.pressed_button.clear();

			// Other buttons and the wheel go to the engine, which only runs a bxt_bingo_board bind
			// (AllowKeyEvent()), so a mouse button bound to the board closes it
			// Press and release are both sent now, so the button is up when the game gets the mouse back
			if (std::strcmp(key_name, "MOUSE1") != 0) {
				int key = KeyNumber(key_name);
				if (key < 0)
					return;

				auto& hw = HwDLL::GetInstance();
				forwarding_key = true;
				bool sent = hw.SendKeyEvent(key, 1);
				if (sent)
					hw.SendKeyEvent(key, 0);
				forwarding_key = false;

				// Without Key_Event, fall back to the first key bound to the board
				if (!sent && IsBoardBind(key_name))
					board.close_requested = true;

				DebugPrint("%s handed to the engine: %s\n", key_name, sent ? "yes" : "no (Key_Event not found)");
				return;
			}

			// Picks the tile if the button went down and up on the same tile
			int hovered = HoveredTile();
			if (hovered >= 0 && hovered == board.pressed_tile) {
				board.selected = hovered;
				board.pick_requested = true;
			}
		}

		void OnKey(Key key)
		{
			DebugPrint("key %d\n", static_cast<int>(key));

			if (!board.open)
				return;

			int row = board.selected / BOARD_SIZE;
			int column = board.selected % BOARD_SIZE;

			switch (key) {
			case Key::UP:
				row = std::max(row - 1, 0);
				break;
			case Key::DOWN:
				row = std::min(row + 1, BOARD_SIZE - 1);
				break;
			case Key::LEFT:
				column = std::max(column - 1, 0);
				break;
			case Key::RIGHT:
				column = std::min(column + 1, BOARD_SIZE - 1);
				break;
			case Key::ENTER:
				board.pick_requested = true;
				break;
			case Key::ESCAPE:
				board.close_requested = true;
				break;
			}

			board.selected = row * BOARD_SIZE + column;
		}

		void OnFocusLost()
		{
			CloseBoard(false);
		}
	}
}
