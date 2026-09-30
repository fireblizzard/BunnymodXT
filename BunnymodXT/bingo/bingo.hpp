#pragma once

// This is the ingame client for the Half-Life bingo community game
// It's inspired by the bingo from Trackmania, which is a 5x5 board of tiles that represent
// a map, and in our case segments of the game too, each with its save and start/end triggers
// BXT reports times and demos and the bxt-bingo-server decides the rest (board, rulesets, winner, etc.)
namespace Bingo
{
	// Call once after the commands are registered, wraps the engine commands bingo needs
	void Init();

	// Call once per frame from HUD_Frame
	// Unlike HUD_Redraw, it also runs while paused and in the main menu
	void Frame();

	// Call from HUD_Redraw, after the rest of the HUD
	void DrawBoard();

	// Draws the mini-board with its top-left corner at (x, y)
	void DrawMiniBoard(int x, int y);

	// Draws the current tile's start and end triggers, from TriangleDrawing::Draw
	void DrawTriggers(struct triangleapi_s* tri);

	// Call with the player's position every frame, and from PM_Move with each move
	void UpdateTriggers(const Vector& player_position, bool ducking);
	void UpdateTriggers(const Vector& player_position_start, const Vector& player_position_end, bool ducking);

	// Call when the game ends (BXT's timer autostop, e.g. Nihilanth's death), before the timer stops
	void OnGameEnd();

	// Call from CBaseMonster::Killed, for the Bloodthirsty handicap
	// attacker is who killed it, or nullptr
	void OnMonsterKilled(const struct entvars_s* monster, const char* classname, const struct entvars_s* attacker);

	// Call with text the player runs from a key or the console, before the engine runs it
	// Returns false when the engine must not run it (a command blocked by a handicap)
	bool OnPlayerCommand(const char* text);

	// Call from HUD_Key_Event, returns false when the engine must skip the key's bind
	// Only filters the mouse buttons the board hands to the engine (see the input rules below)
	bool AllowKeyEvent(const char* binding);

	// Commands
	void Join(const char* code);              // bxt_bingo_join, an empty code rejoins the last game
	void LoadManifest(const char* file_name); // bxt_bingo_manifest
	void ToggleBoard();                       // bxt_bingo_board
	void PlayTile(const char* tile);          // bxt_bingo_play
	void Leave();                             // bxt_bingo_leave
	void PrintStatus();                       // bxt_bingo_status
	void PrintSaveHash(const char* save_name); // _bxt_bingo_hash

	// Debug commands to test the board without a server
	void DebugSetTile(const char* tile, const char* owner, int time_ms, const char* holder); // _bxt_bingo_set_tile
	void DebugSetPlayable(const char* tile, bool playable); // _bxt_bingo_set_playable
	void DebugSetContesting(const char* tile, int red, int blue); // _bxt_bingo_set_contesting
	void DebugEvent(const char* event); // _bxt_bingo_event
	void DebugSetSingleSegment(bool single_segment); // _bxt_bingo_set_single_segment
	void DebugSetTeam(const char* team); // _bxt_bingo_set_team
	void DebugSetTeamColor(const char* team, const char* color); // _bxt_bingo_set_team_color

	// Board input, reported by the platform layer while the board is open:
	// - Mouse movement moves the board's cursor, mouse-look is off
	// - Left click picks a tile on release, the game never sees it
	// - Other mouse buttons and the wheel go to the engine, which only runs a bxt_bingo_board bind
	// - Arrow keys, Enter and Esc are taken by the board
	// - Other keys reach the game, so the player can move and keyboard binds keep working
	namespace Input
	{
		enum class Key
		{
			UP,
			DOWN,
			LEFT,
			RIGHT,
			ENTER,
			ESCAPE
		};

		// Cursor position as a fraction of the window (0 to 1)
		void OnMouseMove(float x, float y);

		// Engine key names: MOUSE1 to MOUSE5, MWHEELUP, MWHEELDOWN
		// The wheel reports a press and a release at once
		void OnMouseButton(const char* key_name, bool down);

		void OnKey(Key key);

		// The game window lost focus or is closing
		void OnFocusLost();

		// Prints to the console when _bxt_bingo_debug_input is 1
		void DebugPrint(const char* format, ...);
	}

	// Platform layer: Windows/bingo_platform.cpp (also HL WON under Wine/Proton)
	// and Linux/bingo_platform.cpp (a stub until HL Steam support)
	namespace Platform
	{
		// Whether this build can connect to a bingo server
		bool IsSupported();

		// Lowercase hex SHA-256 of a file and its size, or an empty string if it can't be read
		std::string Sha256File(const std::string& path, uint64_t& size);

		// Sends mouse and navigation keys to Bingo::Input instead of the game
		// Returns false if this platform can't capture input
		bool CaptureInput();

		// Gives the input back to the game
		void ReleaseInput();

		// Something that happened on a Socket
		struct SocketEvent
		{
			enum class Type
			{
				OPENED,  // connected, messages can be sent
				MESSAGE, // a text message, in text
				REFUSED, // the server answered HTTP code instead of connecting, its body in text
				FAILED,  // the server couldn't be reached, why in text
				CLOSED   // the connection ended with close code (1006 if it dropped), the reason in text
			};

			Type type = Type::FAILED;
			int code = 0;
			std::string text;
		};

		// A WebSocket connection, run by worker threads so the game never waits on the network
		// Only used from the game thread
		class Socket
		{
		public:
			// Closing isn't waited for, the workers finish on their own
			virtual ~Socket() = default;

			// Queued, and sent in order once connected
			virtual void Send(const std::string& text) = 0;

			// Sends what's queued, then closes with the code
			virtual void Close(int code) = 0;

			// The next event, or false if there's none
			virtual bool Poll(SocketEvent& event) = 0;
		};

		// Starts connecting to a ws:// or wss:// URL, with extra request headers ("Name: value")
		std::unique_ptr<Socket> OpenSocket(const std::string& url, const std::vector<std::string>& headers);

		// A file the game needs, and where it goes
		struct FileJob
		{
			std::string url;    // http:// or https://
			std::string path;   // relative to the Half-Life folder, like the game's own paths
			std::string sha256; // lowercase hex
			uint64_t size = 0;
			bool replace = true; // whether a different file already there may be replaced
		};

		// What happened to one of the files
		struct FileEvent
		{
			enum class Type
			{
				PRESENT,    // it was there already
				DOWNLOADED, // downloaded and checked
				CONFLICT,   // a different file is there, and it may not be replaced
				FAILED      // couldn't get it
			};

			Type type = Type::FAILED;
			size_t index = 0; // in the list given to SyncFiles
			std::string text;
		};

		// Checks files and downloads the missing ones on a worker thread, one at a time
		// There's one event per file. Dropping it stops the downloads
		// Only used from the game thread
		class FileSync
		{
		public:
			virtual ~FileSync() = default;

			// The next event, or false if there's none yet
			virtual bool Poll(FileEvent& event) = 0;
		};

		// A file is there when its size and SHA-256 match. A download is checked the same way
		// before it replaces anything, and is tried twice
		std::unique_ptr<FileSync> SyncFiles(std::vector<FileJob> jobs);
	}
}
