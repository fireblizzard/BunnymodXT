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
		// By coordinate (B3) or label (OAR2), ignoring case, or -1
		int FindTile(const char* name)
		{
			int index = ParseTileId(name);
			if (index >= 0)
				return index;

			for (int i = 0; i < TILE_COUNT; ++i) {
				const auto& label = manifest.tiles[i].label;
				if (!label.empty() && boost::iequals(label, name))
					return i;
			}

			return -1;
		}

		constexpr size_t MAX_MESSAGES = 3;

		const CVarWrapper& EventSound(Event event)
		{
			switch (event) {
			case Event::CAPTURE:
				return CVars::bxt_bingo_sound_capture;
			case Event::ALLY_CAPTURE:
				return CVars::bxt_bingo_sound_ally_capture;
			case Event::OPPONENT_CAPTURE:
				return CVars::bxt_bingo_sound_opponent_capture;
			case Event::CONTESTED:
				return CVars::bxt_bingo_sound_contested;
			case Event::INVALID:
				return CVars::bxt_bingo_sound_invalid;
			case Event::REQUIREMENT:
				return CVars::bxt_bingo_sound_requirement;
			case Event::REQUIREMENT_LEFT:
				return CVars::bxt_bingo_sound_requirement_left;
			case Event::WIN:
				break;
			}

			return CVars::bxt_bingo_sound_win;
		}
	}

	Manifest manifest;

	// The tile the player picked last, or -1
	int current_tile = -1;

	// The player's team, NONE offline and while spectating
	Owner my_team = Owner::NONE;

	// The team offline runs count for: the one from _bxt_bingo_set_team, or red
	Owner OfflineTeam()
	{
		return my_team == Owner::NONE ? Owner::RED : my_team;
	}

	// Colors the teams picked in the frontend (red, then blue), empty if not picked
	std::array<std::optional<Rgb>, 2> team_colors;

	// Counts Frame() calls, shown by bxt_bingo_status
	unsigned long long frame_count = 0;

	// Formats here because the engine's Con_Printf on HL WON doesn't support %llu
	void Print(const char* format, ...)
	{
		char buffer[1024];
		va_list args;
		va_start(args, format);
		vsnprintf(buffer, sizeof(buffer), format, args);
		va_end(args);

		HwDLL::GetInstance().ORIG_Con_Printf("%s", buffer);
	}

	std::string GameDir()
	{
		auto& cl = ClientDLL::GetInstance();
		if (!cl.pEngfuncs)
			return {};

		return cl.pEngfuncs->pfnGetGameDirectory();
	}

	// Whether the game runs in a board's game folder. The same given name or one that starts with it
	// plus _, like valve_WON for valve. An empty one is any game
	bool RunsInGame(const std::string& game)
	{
		auto dir = GameDir();
		auto slash = dir.find_last_of("/\\");
		if (slash != std::string::npos)
			dir = dir.substr(slash + 1);

		return game.empty() || boost::iequals(dir, game) || boost::istarts_with(dir, game + "_");
	}

	std::string GameName(const std::string& game)
	{
		static const std::pair<const char*, const char*> NAMES[] = {
			{ "valve", "Half-Life" },
			{ "bshift", "Blue Shift" },
			{ "gearbox", "Opposing Force" },
			{ "ag", "Adrenaline Gamer" },
		};

		for (const auto& pair : NAMES) {
			if (game == pair.first)
				return std::string(pair.second) + " (" + game + ")";
		}

		return game;
	}

	bool IsInMap()
	{
		auto& cl = ClientDLL::GetInstance();
		if (!cl.pEngfuncs)
			return false;

		char map_name[64];
		return cl.GetMapName(map_name, sizeof(map_name)) > 0;
	}

	// A1 is the top-left tile, E5 the bottom-right one
	// Tiles are stored row by row (A1, B1, ..., E5)
	int ParseTileId(const std::string& id)
	{
		if (id.size() != 2)
			return -1;

		int column = std::toupper(static_cast<unsigned char>(id[0])) - 'A';
		int row = id[1] - '1';
		if (column < 0 || column >= BOARD_SIZE || row < 0 || row >= BOARD_SIZE)
			return -1;

		return row * BOARD_SIZE + column;
	}

	std::string TileId(int index)
	{
		return { static_cast<char>('A' + index % BOARD_SIZE), static_cast<char>('1' + index / BOARD_SIZE) };
	}

	// "B3 (OAR2)", or "B3" while the label is hidden
	std::string TileName(int index)
	{
		const auto& label = manifest.tiles[index].label;
		if (label.empty())
			return TileId(index);

		return TileId(index) + " (" + label + ")";
	}

	std::string FormatTime(int time_ms)
	{
		int ms = time_ms % 1000;
		int seconds = time_ms / 1000 % 60;
		int minutes = time_ms / 60000 % 60;
		int hours = time_ms / 3600000;

		char buffer[32];
		if (hours > 0)
			sprintf(buffer, "%d:%02d:%02d.%03d", hours, minutes, seconds, ms);
		else if (minutes > 0)
			sprintf(buffer, "%d:%02d.%03d", minutes, seconds, ms);
		else
			sprintf(buffer, "%d.%03d", seconds, ms);

		return buffer;
	}

	// The latest messages, shown under the mini-board for a while
	std::deque<Message> messages;

	// "fvox/bell" plays sound/fvox/bell.wav, from the game directory or else from valve
	// Empty plays nothing, and a missing file is skipped so the engine doesn't complain
	void PlayEventSound(Event event)
	{
		auto name = EventSound(event).GetString();
		if (name.empty())
			return;
		if (!boost::iends_with(name, ".wav"))
			name += ".wav";

		auto game_dir = GameDir();
		auto slash = game_dir.find_last_of("/\\");
		auto valve_dir = (slash == std::string::npos ? std::string() : game_dir.substr(0, slash + 1)) + "valve";
		if (!FileExists(game_dir + "/sound/" + name) && !FileExists(valve_dir + "/sound/" + name)) {
			EngineDevMsg("[bingo] Sound %s not found.\n", name.c_str());
			return;
		}

		auto engfuncs = ClientDLL::GetInstance().pEngfuncs;
		auto volume = std::clamp(CVars::bxt_bingo_sound_volume.GetFloat(), 0.0f, 1.0f);
		if (engfuncs && volume > 0)
			engfuncs->pfnPlaySoundByName(const_cast<char*>(name.c_str()), volume);
	}

	// Shows it under the mini-board only (bxt_bingo_messages)
	void AddMessage(const std::string& text)
	{
		if (CVars::bxt_bingo_messages.GetBool()) {
			messages.push_back({ text, std::chrono::steady_clock::now() });
			while (messages.size() > MAX_MESSAGES)
				messages.pop_front();
		}
	}

	// Prints to the console and shows it under the mini-board
	void ShowMessage(const std::string& text)
	{
		Print("%s\n", text.c_str());
		AddMessage(text);
	}

	// ShowMessage, and the event's sound
	void Notify(Event event, const std::string& text)
	{
		ShowMessage(text);
		PlayEventSound(event);
	}

	// Swaps an engine command's handler for ours, found by name so no byte patterns are needed
	void WrapCommand(const char* name, xcommand_t& original, xcommand_t wrapper)
	{
		auto& hw = HwDLL::GetInstance();
		if (!hw.ORIG_Cmd_FindCmd)
			return;

		auto cmd = hw.ORIG_Cmd_FindCmd(name);
		if (!cmd || !cmd->function || cmd->function == wrapper)
			return;

		original = cmd->function;
		cmd->function = wrapper;
		EngineDevMsg("[bingo] Wrapped the %s command.\n", name);
	}

	int TeamIndex(Owner team)
	{
		return team == Owner::RED ? 0 : 1;
	}

	const char* TeamName(Owner team)
	{
		switch (team) {
		case Owner::RED:
			return "red";
		case Owner::BLUE:
			return "blue";
		default:
			return "none";
		}
	}

	bool ParseTeam(const char* name, Owner& team)
	{
		if (boost::iequals(name, "red"))
			team = Owner::RED;
		else if (boost::iequals(name, "blue"))
			team = Owner::BLUE;
		else if (boost::iequals(name, "none"))
			team = Owner::NONE;
		else
			return false;

		return true;
	}

	void Init()
	{
		WrapConsoleCommands();
		WrapRunCommands();
	}

	void Frame()
	{
		++frame_count;

		NetFrame();
		DemosFrame();

		// The player's own retry save is still backed up if the game closed while playing a tile
		static bool checked_backup = false;
		if (!checked_backup && ClientDLL::GetInstance().pEngfuncs) {
			checked_backup = true;
			RestoreRetrySave();
		}

		RunFrame();
		BoardFrame();
	}

	void Join(const char* code)
	{
		if (!Platform::IsSupported()) {
			Print("Bingo networking isn't supported on this engine build yet.\n");
			return;
		}

		std::string join_code = code;
		boost::algorithm::trim(join_code);

		if (Online()) {
			if (join_code.empty()) {
				Print("You're already in an online game. Use bxt_bingo_join <code> to join another one.\n");
				return;
			}

			// Joining another game leaves this one, but keeps its session file until the new game lets the player in
			// If the code is refused, bxt_bingo_join without a code still goes back, with the unconfirmed times
			if (!net.pending.empty())
				Print("%d of your times weren't confirmed by the server and won't count if you join the other game.\n", static_cast<int>(net.pending.size()));

			if (net.socket)
				net.socket->Close(1000);
			my_team = Owner::NONE;
			team_colors = {};
			RestoreRetrySave();
			Print("Left the online game.\n");
		}

		net = Net();
		if (join_code.empty()) {
			if (!LoadSession()) {
				net = Net();
				Print("Usage: bxt_bingo_join <code>, with the code from the game's page. Without a code it goes back to the last game, and there's none.\n");
				return;
			}
			Print("Going back to the last game at %s.\n", net.url.c_str());
		} else {
			std::string error;
			net.url = ServerUrl(CVars::bxt_bingo_server.GetString(), error);
			if (net.url.empty()) {
				Print("%s\n", error.c_str());
				return;
			}
			net.join_code = join_code;
			Print("Joining the game at %s.\n", net.url.c_str());
		}

		// The server sends the board, the local one goes
		CancelAttempt("joined an online game", false);
		attempt.state = AttemptState::IDLE;
		ClearTriggers();
		CloseBoard(true);
		manifest = Manifest();
		current_tile = -1;
		messages.clear();

		StartConnection();
	}

	void LoadManifest(const char* file_name)
	{
		if (Online()) {
			Print("You're in an online game, the server sends the board. Use bxt_bingo_leave first.\n");
			return;
		}

		// The .json extension is optional, like .sav for `load`
		// Looks in the Half-Life folder, then in the game directory
		std::string name = file_name;
		std::vector<std::string> names;
		if (!boost::iends_with(name, ".json"))
			names.push_back(name + ".json");
		names.push_back(name);

		std::string path;
		std::string text;
		bool found = false;
		for (const auto& dir : { std::string(), GameDir() + "/" }) {
			for (const auto& candidate : names) {
				path = dir + candidate;
				if (ReadFile(path, text)) {
					found = true;
					break;
				}
			}

			if (found)
				break;
		}

		if (!found) {
			Print("Could not open %s.\n", file_name);
			return;
		}

		Manifest loaded;
		std::string error;
		if (!ParseManifest(text, loaded, error)) {
			Print("Could not load %s: %s.\n", path.c_str(), error.c_str());
			return;
		}

		if (!RunsInGame(loaded.game)) {
			Print("%s is played in %s, and this is %s.\n", path.c_str(), GameName(loaded.game).c_str(), GameDir().c_str());
			return;
		}

		CancelAttempt("loaded another board", false);
		attempt.state = AttemptState::IDLE;
		ClearTriggers();

		manifest = std::move(loaded);
		current_tile = -1;
		Print("Loaded the board \"%s\" from %s.\n", manifest.hash.c_str(), path.c_str());
	}

	void PlayTile(const char* tile_name)
	{
		if (!manifest.loaded) {
			Print("No board is loaded. Use bxt_bingo_manifest <file>.\n");
			return;
		}

		int index = FindTile(tile_name);
		if (index < 0) {
			Print("There's no tile %s on the board.\n", tile_name);
			return;
		}

		PickTile(index);
	}

	void Leave()
	{
		if (Online()) {
			if (!net.pending.empty())
				Print("%d of your times weren't confirmed by the server and won't count.\n", static_cast<int>(net.pending.size()));

			if (net.socket)
				net.socket->Close(1000);
			ClearSession();
			net = Net();
			my_team = Owner::NONE;
			team_colors = {};
			Print("Left the online game.\n");
		}

		CancelAttempt("left the board", false);
		attempt.state = AttemptState::IDLE;
		ClearTriggers();
		CloseBoard(true);

		manifest = Manifest();
		current_tile = -1;
		RestoreRetrySave();
		Print("Left the board.\n");
	}

	void PrintStatus()
	{
		Print("Bingo status:\n");

		if (Platform::IsSupported())
			Print(" Networking: available\n");
		else
			Print(" Networking: not supported on this engine build yet\n");

		auto server = CVars::bxt_bingo_server.GetString();
		if (server.empty())
			Print(" Server: not set (bxt_bingo_server)\n");
		else
			Print(" Server: %s\n", server.c_str());

		if (Online()) {
			Print(" Online game: %s (%s)\n", MatchStatus().c_str(), net.url.c_str());
			if (!net.name.empty())
				Print(" You: %s\n", net.name.c_str());
			for (const auto& player : net.players)
				Print("  %s: %s%s%s\n", TeamName(player.team), player.name.c_str(), player.ready ? ", ready" : "", player.connected ? "" : ", not connected");
			if (!net.file_jobs.empty())
				Print(" Files: %d of %d here\n", FilesReady(), static_cast<int>(net.file_jobs.size()));
			if (!net.pending.empty())
				Print(" Times waiting for the server: %d\n", static_cast<int>(net.pending.size()));
		} else
			Print(" Online game: none (bxt_bingo_join)\n");

		Print(" Retry save: %s\n", CVars::bxt_bingo_retry_save.GetString().c_str());
		Print(" Run type: %s\n", RunType().c_str());
		auto handicaps = Handicaps();
		Print(" Handicaps: %s\n", handicaps.empty() ? "none" : handicaps.c_str());

		if (manifest.loaded)
			Print(" Board: %s\n", manifest.hash.c_str());
		else
			Print(" Board: none loaded (bxt_bingo_manifest)\n");

		if (current_tile >= 0) {
			Print(" Current tile: %s\n", TileName(current_tile).c_str());

			const char* states[] = { "none", "loading its save", "waiting for the start trigger", "running", "finished" };
			Print(" Attempt: %s", states[static_cast<int>(attempt.state)]);
			if (attempt.state == AttemptState::RUNNING || attempt.state == AttemptState::FINISHED)
				Print(", %s", FormatTime(TimerMs()).c_str());
			Print("\n");
		}

		Print(" Your team: %s\n", TeamName(my_team));
		for (auto team : { Owner::RED, Owner::BLUE }) {
			std::string source;
			auto color = TeamColor(team, &source);
			Print(" Color of team %s: %d %d %d (%s)\n", TeamName(team), color.r, color.g, color.b, source.c_str());
		}

		Print(" Frames: %llu\n", frame_count);
	}

	void PrintSaveHash(const char* save_name)
	{
		auto path = SavePath(save_name);
		if (path.empty()) {
			Print("The game directory isn't known yet.\n");
			return;
		}

		uint64_t size;
		auto hash = Platform::Sha256File(path, size);
		if (hash.empty())
			Print("%s: could not read or hash the file.\n", path.c_str());
		else
			Print("%s: %llu bytes, SHA-256 %s\n", path.c_str(), static_cast<unsigned long long>(size), hash.c_str());
	}

	void DebugSetTile(const char* tile_name, const char* owner, int time_ms, const char* holder)
	{
		if (!manifest.loaded) {
			Print("No board is loaded.\n");
			return;
		}

		int index = FindTile(tile_name);
		if (index < 0) {
			Print("There's no tile %s on the board.\n", tile_name);
			return;
		}

		Owner new_owner;
		if (!ParseTeam(owner, new_owner)) {
			Print("The owner must be red, blue or none.\n");
			return;
		}

		SetTileOwner(index, new_owner, time_ms, holder);
	}

	void DebugSetContesting(const char* tile_name, int red, int blue)
	{
		if (!manifest.loaded) {
			Print("No board is loaded.\n");
			return;
		}

		int index = FindTile(tile_name);
		if (index < 0) {
			Print("There's no tile %s on the board.\n", tile_name);
			return;
		}

		std::vector<Owner> contesting(std::max(0, red), Owner::RED);
		contesting.insert(contesting.end(), std::max(0, blue), Owner::BLUE);
		SetContesting(index, std::move(contesting));
	}

	void DebugEvent(const char* name)
	{
		static const std::pair<const char*, Event> EVENTS[] = {
			{ "capture", Event::CAPTURE },
			{ "ally_capture", Event::ALLY_CAPTURE },
			{ "opponent_capture", Event::OPPONENT_CAPTURE },
			{ "contested", Event::CONTESTED },
			{ "invalid", Event::INVALID },
			{ "win", Event::WIN },
		};

		for (const auto& pair : EVENTS) {
			if (boost::iequals(name, pair.first)) {
				Notify(pair.second, std::string("Test message for ") + pair.first + ".");
				return;
			}
		}

		Print("The event must be capture, ally_capture, opponent_capture, contested, invalid or win.\n");
	}

	void DebugSetPlayable(const char* tile_name, bool playable)
	{
		if (!manifest.loaded) {
			Print("No board is loaded.\n");
			return;
		}

		int index = FindTile(tile_name);
		if (index < 0) {
			Print("There's no tile %s on the board.\n", tile_name);
			return;
		}

		manifest.tiles[index].playable = playable;
	}

	void DebugSetSingleSegment(bool single_segment)
	{
		manifest.ruleset.single_segment = single_segment;
	}

	void DebugSetTeam(const char* team)
	{
		if (!ParseTeam(team, my_team))
			Print("The team must be red, blue or none.\n");
	}

	void DebugSetTeamColor(const char* team_name, const char* color_text)
	{
		Owner team;
		if (!ParseTeam(team_name, team) || team == Owner::NONE) {
			Print("The team must be red or blue.\n");
			return;
		}

		if (!*color_text) {
			team_colors[TeamIndex(team)].reset();
			return;
		}

		Rgb color;
		if (!ParseColor(color_text, color)) {
			Print("The color must be \"R G B\" (0 to 255 each) or #rrggbb.\n");
			return;
		}

		team_colors[TeamIndex(team)] = color;
	}
}
