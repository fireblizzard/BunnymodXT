#pragma once

#include <deque>
#include <optional>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "../custom_triggers.hpp"
#include "bingo.hpp"
#include "bingo_rules.hpp"

// What the bingo files share with each other, the rest of BXT uses bingo.hpp
// Each file keeps what only it uses in an anonymous namespace
//
// bingo.cpp          the commands, Init() and Frame(), the loaded board, messages and sounds
// bingo_manifest.cpp reading the manifest
// bingo_saves.cpp    the tiles' saves and the retry save
// bingo_run.cpp      attempts: loading a tile, its triggers, the timer, the rules, saving and loading during a run
// bingo_net.cpp      the connection to the bingo server and its messages
// bingo_files.cpp    downloading the manifest's files
// bingo_draw.cpp     drawing the board, the mini-board and the triggers
// bingo_input.cpp    opening and closing the board, and its mouse and keys
// bingo_rules.cpp    rulesets and handicaps (bingo_rules.hpp)

namespace Bingo
{
	// bingo.cpp
	constexpr int BOARD_SIZE = 5;  // could be customizable in the future, but requires a bunch of changes
	constexpr int TILE_COUNT = BOARD_SIZE * BOARD_SIZE;

	enum class Owner
	{
		NONE,
		RED,
		BLUE
	};

	struct TriggerBox
	{
		std::string map; // empty for any map
		std::array<std::array<float, 3>, 2> corners = {};
	};

	struct Tile
	{
		// From the manifest
		std::string label; // empty while hidden until the round starts
		std::string save_sha256;
		uint64_t save_size = 0;
		bool start_on_load = false; // otherwise the start trigger starts the timer
		bool end_on_game_end = false; // otherwise the end trigger stops the timer, e.g. Nihilanth's death
		TriggerBox start;
		TriggerBox end;

		// Board state, from the server
		Owner owner = Owner::NONE;
		int time_ms = -1; // -1 if nobody has a time yet
		std::string holder;
		bool playable = true;

		// The team of each player who picked this tile, empty when the lobby turns contesting off
		std::vector<Owner> contesting;
	};

	// A file the game needs besides the saves, e.g. the win sound
	struct ExtraFile
	{
		std::string path; // in the game directory
		std::string sha256;
		uint64_t size = 0;
	};

	struct Manifest
	{
		bool loaded = false;
		std::string hash;

		Rules::Ruleset ruleset;

		std::array<Tile, TILE_COUNT> tiles;

		std::vector<ExtraFile> extra_files;

		// The game folder it's played in, e.g. valve, empty if the manifest doesn't say
		std::string game;

		// Files are downloaded from <files_url><sha256>
		// A path like /files/ is on the server BXT connected to, and empty means /files/
		std::string files_url;
	};

	struct Rgb
	{
		int r;
		int g;
		int b;
	};

	// Things that happen in the game, each with its own sound cvar
	enum class Event
	{
		CAPTURE,          // you took a tile or beat its time
		ALLY_CAPTURE,     // someone else on your team took a tile
		OPPONENT_CAPTURE, // the other team took a tile
		CONTESTED,        // an opponent picked the tile you're playing
		INVALID,          // your run was cancelled or no longer counts
		WIN               // a team won the game
	};

	struct Message
	{
		std::string text;
		std::chrono::steady_clock::time_point shown;
	};

	// How long a message stays under the mini-board
	constexpr long long MESSAGE_MS = 6000;

	extern Manifest manifest;
	extern int current_tile;
	extern Owner my_team;
	extern std::array<std::optional<Rgb>, 2> team_colors;
	extern unsigned long long frame_count;
	extern std::deque<Message> messages;

	Owner OfflineTeam();
	void Print(const char* format, ...);
	std::string GameDir();
	bool RunsInGame(const std::string& game);
	std::string GameName(const std::string& game);
	bool IsInMap();
	int ParseTileId(const std::string& id);
	std::string TileId(int index);
	std::string TileName(int index);
	std::string FormatTime(int time_ms);
	void PlayEventSound(Event event);
	void AddMessage(const std::string& text);
	void ShowMessage(const std::string& text);
	void Notify(Event event, const std::string& text);
	void WrapCommand(const char* name, xcommand_t& original, xcommand_t wrapper);
	int TeamIndex(Owner team);
	const char* TeamName(Owner team);
	bool ParseTeam(const char* name, Owner& team);

	// bingo_input.cpp
	struct Board
	{
		bool open = false;
		int selected = TILE_COUNT / 2; // starts in the middle

		// Cursor position as a fraction of the window, once the mouse has moved
		bool mouse_seen = false;
		float mouse_x = 0;
		float mouse_y = 0;

		// Set by the input handlers and carried out by Frame()
		bool close_requested = false;
		bool pick_requested = false;

		// Clicks act on release, so the button is up when the game gets the mouse back
		// (acting on the press made a click on a tile also shoot)
		std::string pressed_button;
		int pressed_tile = -1;

		// crosshair cvar value before the board hid it, or 0
		float saved_crosshair = 0;

		unsigned long long last_drawn_frame = 0;
		std::chrono::steady_clock::time_point last_drawn_time;

		// Whether the player was seen outside a map since the last draw, for the debug log
		bool left_map_since_drawn = false;

		// Last client state seen while the board is open, for the debug log
		int client_state = -1;
	};

	extern Board board;

	void CloseBoard(bool restore_mouse);
	void WrapConsoleCommands();
	void BoardFrame();

	// bingo_saves.cpp
	struct FileHash
	{
		std::string sha256; // Empty if the file couldn't be read
		uint64_t size = 0;

		bool operator==(const FileHash& other) const
		{
			return sha256 == other.sha256 && size == other.size;
		}
	};

	std::string SavePath(const char* save_name);
	bool ReadFile(const std::string& path, std::string& contents);
	std::string TileSaveName(const Tile& tile);
	std::string RetrySaveName();
	std::string SaveNameArg();
	bool FileExists(const std::string& path);
	bool CopyFile(const std::string& from, const std::string& to);
	FileHash HashSave(const std::string& name);
	bool IsTileSave(const std::string& path, const Tile& tile);
	void BackUpRetrySave(const std::string& retry_save);
	void RestoreRetrySave();

	// bingo_run.cpp
	// An attempt at the current tile
	// LOADING (its save is loading) -> ARMED (waiting for the start trigger) -> RUNNING -> FINISHED
	// Loading the retry save again starts over, anything that could cheat a run cancels it (IDLE)
	enum class AttemptState
	{
		IDLE,
		LOADING,
		ARMED,
		RUNNING,
		FINISHED
	};

	struct Attempt
	{
		AttemptState state = AttemptState::IDLE;

		// Whether the client left the game while the save loads, so ca_active again means it's loaded
		bool saw_loading = false;
		std::chrono::steady_clock::time_point load_started;

		// Why the run no longer counts, while it keeps going (e.g. took damage in a "No Damage%" run)
		std::string invalid_reason;

		// Online: the attempt_id sent at the start trigger, empty before that
		std::string id;
		std::chrono::steady_clock::time_point started_at;

		// Its result or attempt_invalidated was sent
		bool reported = false;

		// Enemy monsters the player killed since the start trigger, for Bloodthirsty
		int kills = 0;
	};

	// A tile's start or end trigger, only counted on its map (any map when the map is empty)
	class TileTrigger : public CustomTriggers::Trigger
	{
	public:
		TileTrigger(const TriggerBox& box, bool is_end)
			: CustomTriggers::Trigger(
				Vector(box.corners[0][0], box.corners[0][1], box.corners[0][2]),
				Vector(box.corners[1][0], box.corners[1][1], box.corners[1][2]))
			, map(box.map)
			, is_end(is_end)
		{
		}

		// Whether it's on the current map
		bool counts_here() const;

	protected:
		void touch() override;

	private:
		std::string map;
		bool is_end;
	};

	extern Attempt attempt;
	extern std::optional<TileTrigger> start_trigger;
	extern std::optional<TileTrigger> end_trigger;

	bool IsAttemptActive();
	int TimerMs();
	void CancelAttempt(const char* reason, bool notify = true);
	std::string RunType();
	std::string Handicaps();
	void PickTile(int index);
	void WrapRunCommands();
	void RunFrame();

	// bingo_manifest.cpp
	bool GetString(const rapidjson::Value& object, const char* name, std::string& out, std::string& error);
	bool ParseManifest(const std::string& text, Manifest& result, std::string& error);

	// bingo_net.cpp
	enum class NetState
	{
		OFFLINE,    // no server, the board comes from bxt_bingo_manifest
		CONNECTING, // waiting for the connection, then for welcome
		JOINED,     // in the game
		RETRYING    // the connection dropped, trying again after a pause
	};

	struct LobbyPlayer
	{
		std::string steamid64;
		std::string name;
		Owner team = Owner::NONE;
		bool ready = false;
		bool connected = false;
	};

	// A result the server hasn't acknowledged yet, sent again after a reconnect
	struct PendingResult
	{
		std::string attempt_id;
		int tile = -1;
		int time_ms = 0;
		std::string message;
	};

	struct Net
	{
		NetState state = NetState::OFFLINE;
		std::unique_ptr<Platform::Socket> socket;
		bool opened = false; // connected, before or after welcome

		std::string url;
		std::string join_code;     // for the first connection
		std::string session_token; // from welcome, to reconnect

		// The player, from welcome
		std::string steamid64;
		std::string name;

		int retry_ms = 0;
		std::chrono::steady_clock::time_point retry_at;
		std::chrono::steady_clock::time_point last_ping;
		std::chrono::steady_clock::time_point last_heard;

		// From lobby: lobby, countdown, running or finished
		std::string lobby_state;
		std::vector<LobbyPlayer> players;

		// From round_start
		std::chrono::steady_clock::time_point starts_at;

		// From board: the match clock when it arrived
		long long board_seq = -1;
		long long clock_ms = 0;
		std::chrono::steady_clock::time_point clock_received;
		long long time_limit_ms = -1;   // -1 without a time limit
		long long sudden_death_ms = -1; // -1 without sudden death

		// How the game ended, empty while it goes on
		std::string ending;

		// The first board after joining brings the whole game so far, which isn't news
		bool quiet_board = true;

		std::vector<PendingResult> pending;

		// The manifest's files (BINGO.md §3.2): checked, then downloaded if they're missing
		// ready is sent once they're all there
		std::vector<Platform::FileJob> file_jobs;
		std::vector<std::string> file_names; // for messages, e.g. "the save of B3"
		std::vector<bool> file_ok;
		std::vector<std::string> file_problems; // the last problem shown for each file
		std::vector<std::string> new_file_problems; // found by the running sync, shown at its end
		std::unique_ptr<Platform::FileSync> files;
		std::vector<size_t> file_sync_jobs; // the jobs the running sync has, in its order
		size_t file_events = 0;             // events from the running sync so far
		int files_downloaded = 0;
		bool files_retry = false;
		std::chrono::steady_clock::time_point files_retry_at;
	};

	using JsonWriter = rapidjson::Writer<rapidjson::StringBuffer>;

	// A message as JSON text, written by the function
	template<typename F>
	std::string JsonMessage(const char* type, F write)
	{
		rapidjson::StringBuffer buffer;
		JsonWriter writer(buffer);
		writer.StartObject();
		writer.Key("type");
		writer.String(type);
		write(writer);
		writer.EndObject();
		return buffer.GetString();
	}

	extern Net net;

	bool Online();
	void NetSend(const std::string& message);
	std::string NewAttemptId();
	void ClearSession();
	bool LoadSession();
	void SendTileSelected(int index);
	void SendAttemptStarted();
	void SendAttemptInvalidated(const std::string& reason);
	void SendAttemptResult(int time_ms, long long real_ms);
	void SetTileOwner(int index, Owner owner, int time_ms, const std::string& holder);
	void SetContesting(int index, std::vector<Owner> teams);
	std::string ServerUrl(std::string server, std::string& error);
	void StartConnection();
	void NetFrame();

	// bingo_files.cpp
	int FilesReady();
	void StartFiles();
	void FilesFrame();

	// bingo_draw.cpp
	int HoveredTile();
	bool ParseColor(const std::string& text, Rgb& color);
	Rgb TeamColor(Owner team, std::string* source = nullptr);
	std::string MatchStatus();
}
