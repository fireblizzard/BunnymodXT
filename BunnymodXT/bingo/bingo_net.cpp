#include "../stdafx.hpp"

#include <random>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>

#include "../modules/ClientDLL.hpp"
#include "../modules/HwDLL.hpp"
#include "../git_revision.hpp"
#include "bingo_internal.hpp"

// Networking (BINGO.md §4.2, §6): one WebSocket to the bingo server
// The platform layer runs it on worker threads, and Frame() handles what comes in on the game thread

namespace Bingo
{
	namespace
	{
		void StopNet(const std::string& reason, bool keep_session);

		std::string PlayerName()
		{
			auto engfuncs = ClientDLL::GetInstance().pEngfuncs;
			auto name = engfuncs ? engfuncs->pfnGetCvarString(const_cast<char*>("name")) : nullptr;
			return name ? name : "";
		}

		// BXT pings every 30 s, and the server answers each one
		constexpr long long PING_INTERVAL_MS = 30000;

		// With nothing from the server for this long, the connection is dead
		constexpr long long SILENCE_LIMIT_MS = 75000;

		// Reconnecting waits 1 s, then twice as long each time, up to 30 s
		constexpr int MAX_RETRY_MS = 30000;

		constexpr int PROTOCOL_VERSION = 1;

		// The player's name as the board shows it: the Steam name online, the name cvar offline
		std::string MyName()
		{
			return Online() && !net.name.empty() ? net.name : PlayerName();
		}

		// Where the session and the unacknowledged results are kept, so a crash loses nothing
		std::string SessionPath()
		{
			auto game_dir = GameDir();
			return game_dir.empty() ? std::string() : game_dir + "/bingo_session.json";
		}

		void SaveSession()
		{
			auto path = SessionPath();
			if (path.empty() || net.session_token.empty())
				return;

			rapidjson::StringBuffer buffer;
			JsonWriter writer(buffer);
			writer.StartObject();
			writer.Key("url");
			writer.String(net.url.c_str());
			writer.Key("session_token");
			writer.String(net.session_token.c_str());
			writer.Key("pending");
			writer.StartArray();
			for (const auto& result : net.pending) {
				writer.StartObject();
				writer.Key("tile");
				writer.Int(result.tile);
				writer.Key("time_ms");
				writer.Int(result.time_ms);
				writer.Key("message");
				writer.String(result.message.c_str());
				writer.EndObject();
			}
			writer.EndArray();
			writer.EndObject();

			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			file << buffer.GetString();
		}

		// The engine build the saves have to match, like HL WON (build 3248) or HL Steam
		const char* EngineBuild()
		{
			auto& hw = HwDLL::GetInstance();
			int build = hw.ORIG_build_number ? hw.ORIG_build_number() : -1;
			return build >= 6000 ? "steam" : "won";
		}

		void SendHello()
		{
			// The server takes at most 64 characters
			std::string version = Git::GetRevision();
			if (version.size() > 64)
				version.resize(64);

			auto message = JsonMessage("hello", [&](JsonWriter& w) {
				w.Key("protocol");
				w.Int(PROTOCOL_VERSION);
				w.Key("bxt_version");
				w.String(version.c_str());
				w.Key("engine_build");
				w.String(EngineBuild());
				w.Key("dll_sha256");
				w.Null();
				w.Key("steamid64");
				w.Null();
			});

			// Before welcome, so not through NetSend
			if (net.socket)
				net.socket->Send(message);
		}

		std::string JsonString(const rapidjson::Value& object, const char* name)
		{
			auto it = object.FindMember(name);
			return it != object.MemberEnd() && it->value.IsString() ? std::string(it->value.GetString(), it->value.GetStringLength()) : std::string();
		}

		long long JsonInt(const rapidjson::Value& object, const char* name, long long fallback)
		{
			auto it = object.FindMember(name);
			return it != object.MemberEnd() && it->value.IsInt64() ? it->value.GetInt64() : fallback;
		}

		bool JsonBool(const rapidjson::Value& object, const char* name)
		{
			auto it = object.FindMember(name);
			return it != object.MemberEnd() && it->value.IsBool() && it->value.GetBool();
		}

		Owner JsonTeam(const rapidjson::Value& object, const char* name)
		{
			Owner team = Owner::NONE;
			auto text = JsonString(object, name);
			if (!text.empty())
				ParseTeam(text.c_str(), team);
			return team;
		}

		// Names come from Steam, so control characters are dropped before drawing them
		std::string CleanText(std::string text)
		{
			text.erase(std::remove_if(text.begin(), text.end(), [](char c) {
				return static_cast<unsigned char>(c) < 32 || c == 127;
			}), text.end());
			return text;
		}

		// Whether a host is on this computer or the local network, where servers don't have TLS
		bool IsLocalHost(const std::string& host)
		{
			auto starts = [&](const char* prefix) { return boost::istarts_with(host, prefix); };
			if (boost::iequals(host, "localhost") || starts("127.") || starts("10.") || starts("192.168.") || host == "[::1]"
				|| boost::iends_with(host, ".local") || boost::iends_with(host, ".lan"))
				return true;

			// 172.16.0.0 to 172.31.255.255
			int second = 0;
			return std::sscanf(host.c_str(), "172.%d.", &second) == 1 && second >= 16 && second <= 31;
		}

		// Tries again after a pause, if there's a session to go back to
		void ConnectionLost(const std::string& reason)
		{
			net.socket.reset();
			net.opened = false;

			if (net.session_token.empty()) {
				StopNet("Could not join the game: " + reason + ".", false);
				return;
			}

			net.retry_ms = net.retry_ms == 0 ? 1000 : std::min(net.retry_ms * 2, MAX_RETRY_MS);
			net.retry_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(net.retry_ms);
			net.state = NetState::RETRYING;
			Print("Lost the connection to the bingo server (%s), trying again in %d s.\n", reason.c_str(), net.retry_ms / 1000);
		}

		void OnWelcome(const rapidjson::Value& doc)
		{
			auto player = doc.FindMember("player");
			if (player == doc.MemberEnd() || !player->value.IsObject())
				return;

			bool rejoined = !net.session_token.empty();
			net.session_token = JsonString(doc, "session_token");
			net.steamid64 = JsonString(player->value, "steamid64");
			net.name = CleanText(JsonString(player->value, "name"));
			my_team = JsonTeam(player->value, "team");
			net.state = NetState::JOINED;
			net.retry_ms = 0;
			net.join_code.clear();
			net.quiet_board = true;
			SaveSession();

			if (rejoined)
				Print("Back in the game.\n");
			else
				ShowMessage("Joined the bingo game as " + net.name + (my_team == Owner::NONE ? ", waiting for a team." : " on team " + std::string(TeamName(my_team)) + "."));

			for (const auto& result : net.pending)
				NetSend(result.message);
			if (current_tile >= 0)
				SendTileSelected(current_tile);
		}

		void OnLobby(const rapidjson::Value& doc)
		{
			auto state = JsonString(doc, "state");
			if (state == "running" && net.lobby_state != "running" && !net.lobby_state.empty())
				ShowMessage("Go!");
			net.lobby_state = state;

			net.players.clear();
			auto players = doc.FindMember("players");
			if (players != doc.MemberEnd() && players->value.IsArray()) {
				for (const auto& value : players->value.GetArray()) {
					if (!value.IsObject())
						continue;

					LobbyPlayer player;
					player.steamid64 = JsonString(value, "steamid64");
					player.name = CleanText(JsonString(value, "name"));
					player.team = JsonTeam(value, "team");
					player.ready = JsonBool(value, "ready");
					player.connected = JsonBool(value, "connected");

					if (player.steamid64 == net.steamid64 && player.team != my_team) {
						my_team = player.team;
						ShowMessage(my_team == Owner::NONE ? "You're not on a team now." : "You're on team " + std::string(TeamName(my_team)) + " now.");
					}

					net.players.push_back(std::move(player));
				}
			}

			// The colors the teams picked on the website, or the defaults
			team_colors = {};
			auto teams = doc.FindMember("teams");
			if (teams != doc.MemberEnd() && teams->value.IsArray()) {
				for (const auto& value : teams->value.GetArray()) {
					if (!value.IsObject())
						continue;

					auto team = JsonTeam(value, "team");
					Rgb color;
					if (team != Owner::NONE && ParseColor(JsonString(value, "color"), color))
						team_colors[TeamIndex(team)] = color;
				}
			}
		}

		void OnManifest(const std::string& text)
		{
			Manifest loaded;
			std::string error;
			if (!ParseManifest(text, loaded, error)) {
				Print("The server's board can't be used: %s.\n", error.c_str());
				return;
			}

			// Its saves don't load in another game
			if (!RunsInGame(loaded.game)) {
				StopNet("This game is played in " + GameName(loaded.game) + ", and this is " + GameDir()
					+ ". Start that game and join again with a new code.", false);
				return;
			}

			// The board so far stays, a new manifest only changes the rules or the files
			for (int i = 0; i < TILE_COUNT; ++i) {
				const auto& old = manifest.tiles[i];
				auto& tile = loaded.tiles[i];
				tile.owner = old.owner;
				tile.time_ms = old.time_ms;
				tile.holder = old.holder;
				tile.playable = manifest.loaded ? old.playable : false;
				tile.contesting = old.contesting;
			}

			if (current_tile >= 0 && loaded.tiles[current_tile].save_sha256 != manifest.tiles[current_tile].save_sha256) {
				CancelAttempt("the tile changed", false);
				current_tile = -1;
			}

			bool changed = loaded.hash != manifest.hash;
			manifest = std::move(loaded);
			if (changed) {
				Print("Got the board from the server (%s, %s).\n", RunType().c_str(), manifest.hash.c_str());
				auto handicaps = Handicaps();
				if (!handicaps.empty())
					Print("Your handicaps: %s.\n", handicaps.c_str());
			}

			StartFiles();
		}

		void OnRoundStart(const rapidjson::Value& doc)
		{
			auto countdown_ms = std::max<long long>(0, JsonInt(doc, "countdown_ms", 0));
			net.starts_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(countdown_ms);

			// Labels that were hidden until now
			auto labels = doc.FindMember("labels");
			if (labels != doc.MemberEnd() && labels->value.IsArray()) {
				for (const auto& value : labels->value.GetArray()) {
					if (!value.IsObject())
						continue;

					int index = ParseTileId(JsonString(value, "tile"));
					if (index >= 0)
						manifest.tiles[index].label = JsonString(value, "label");
				}
			}

			ShowMessage("The game starts in " + std::to_string((countdown_ms + 999) / 1000) + " s.");
		}

		void OnBoard(const rapidjson::Value& doc)
		{
			auto tiles = doc.FindMember("tiles");
			if (!manifest.loaded || tiles == doc.MemberEnd() || !tiles->value.IsArray())
				return;

			auto seq = JsonInt(doc, "seq", -1);
			if (seq <= net.board_seq)
				return;
			net.board_seq = seq;

			net.clock_ms = JsonInt(doc, "clock_ms", 0);
			net.clock_received = std::chrono::steady_clock::now();
			net.time_limit_ms = JsonInt(doc, "time_limit_ms", -1);
			net.sudden_death_ms = JsonInt(doc, "sudden_death_ms", -1);

			bool quiet = net.quiet_board;
			net.quiet_board = false;

			for (const auto& value : tiles->value.GetArray()) {
				if (!value.IsObject())
					continue;

				int index = ParseTileId(JsonString(value, "id"));
				if (index < 0)
					continue;

				auto& tile = manifest.tiles[index];
				auto owner = JsonTeam(value, "owner");
				auto time_ms = static_cast<int>(JsonInt(value, "time_ms", -1));
				auto holder = CleanText(JsonString(value, "holder"));
				if (quiet) {
					tile.owner = owner;
					tile.time_ms = time_ms;
					tile.holder = holder;
				} else
					SetTileOwner(index, owner, time_ms, holder);
				tile.playable = JsonBool(value, "playable_for_you");

				std::vector<Owner> contesting;
				auto list = value.FindMember("contesting");
				if (list != value.MemberEnd() && list->value.IsArray()) {
					for (const auto& player : list->value.GetArray()) {
						if (player.IsObject())
							contesting.push_back(JsonTeam(player, "team"));
					}
				}
				if (quiet)
					tile.contesting = std::move(contesting);
				else
					SetContesting(index, std::move(contesting));
			}
		}

		void OnResultAck(const rapidjson::Value& doc)
		{
			auto id = JsonString(doc, "attempt_id");
			auto it = std::find_if(net.pending.begin(), net.pending.end(), [&](const PendingResult& result) { return result.attempt_id == id; });
			if (it == net.pending.end())
				return;

			auto result = *it;
			net.pending.erase(it);
			SaveSession();

			auto verdict = JsonString(doc, "verdict");
			auto detail = CleanText(JsonString(doc, "detail"));
			auto run = FormatTime(result.time_ms) + " on " + (result.tile >= 0 ? TileName(result.tile) : "a tile");
			auto review = JsonBool(doc, "flagged") ? " It's held for the host's review" + (detail.empty() ? std::string() : " (" + detail + ")") + "." : std::string();

			if (verdict == "captured")
				Notify(Event::CAPTURE, "Took " + run + "." + review);
			else if (verdict == "stolen")
				Notify(Event::CAPTURE, "Stole " + run + "." + review);
			else if (verdict == "improved")
				Notify(Event::CAPTURE, "Improved " + run + "." + review);
			else if (verdict == "not_faster")
				ShowMessage(run + " wasn't faster than the time to beat.");
			else if (verdict == "locked")
				ShowMessage(run + " didn't count, your team can't play that tile now.");
			else if (verdict == "game_over")
				ShowMessage(run + " didn't count, the game was over.");
			else
				Notify(Event::INVALID, run + " was rejected" + (detail.empty() ? "." : ": " + detail + "."));
		}

		void OnGameOver(const rapidjson::Value& doc)
		{
			auto winner = JsonTeam(doc, "winner");
			auto reason = JsonString(doc, "reason");
			auto team = "Team " + std::string(TeamName(winner));
			if (reason == "draw")
				net.ending = "Draw";
			else if (reason == "host_ended")
				net.ending = "Ended by the host";
			else if (winner == Owner::NONE)
				net.ending = "Game over";
			else
				net.ending = team + " wins";

			CancelAttempt("the game is over", false);
			PlayEventSound(Event::WIN);
		}

		void OnMessage(const std::string& text)
		{
			rapidjson::Document doc;
			doc.Parse(text.c_str());
			if (doc.HasParseError() || !doc.IsObject()) {
				EngineDevMsg("[bingo] The server sent something that isn't a JSON object.\n");
				return;
			}

			auto type = JsonString(doc, "type");
			if (type == "welcome")
				OnWelcome(doc);
			else if (type == "pong")
				return;
			else if (type == "lobby")
				OnLobby(doc);
			else if (type == "manifest")
				OnManifest(text);
			else if (type == "round_start")
				OnRoundStart(doc);
			else if (type == "board")
				OnBoard(doc);
			else if (type == "event")
				ShowMessage(CleanText(JsonString(doc, "text")));
			else if (type == "result_ack")
				OnResultAck(doc);
			else if (type == "request_demo")
				EngineDevMsg("[bingo] The server asked for a demo, which comes with evidence (BINGO.md §9 step 7).\n");
			else if (type == "game_over")
				OnGameOver(doc);
			else if (type == "error")
				Print("Bingo server: %s (%s).\n", CleanText(JsonString(doc, "detail")).c_str(), JsonString(doc, "code").c_str());
			else
				EngineDevMsg("[bingo] Unknown message from the server: %s.\n", type.c_str());
		}

		// Why the server refused the connection, from its {"error": "..."} body
		std::string RefusalText(int status, const std::string& body)
		{
			rapidjson::Document doc;
			doc.Parse(body.c_str());
			auto code = !doc.HasParseError() && doc.IsObject() ? JsonString(doc, "error") : std::string();

			if (code == "bad_code")
				return "the join code is wrong or was already used, get a new one on the website";
			if (code == "code_expired")
				return "the join code expired, get a new one on the website";
			if (code == "bad_session")
				return "the game doesn't know this session any more, join again with a new code";
			if (code == "game_locked")
				return "the game is locked";
			if (code == "game_full")
				return "the game is full";
			if (code == "game_over")
				return "the game is over";
			if (code == "banned")
				return "you're banned from this game";
			return "the server answered HTTP " + std::to_string(status);
		}

		void OnSocketEvent(const Platform::SocketEvent& event)
		{
			using Type = Platform::SocketEvent::Type;
			auto now = std::chrono::steady_clock::now();

			switch (event.type) {
			case Type::OPENED:
				net.opened = true;
				net.last_heard = now;
				net.last_ping = now;
				SendHello();
				break;

			case Type::MESSAGE:
				net.last_heard = now;
				OnMessage(event.text);
				break;

			case Type::REFUSED:
				// Too many tries or a server error: wait and try again, anything else won't get better by trying
				if (event.code == 429)
					ConnectionLost("too many tries");
				else if (event.code >= 500)
					ConnectionLost("server error " + std::to_string(event.code));
				else
					StopNet("Could not join the game: " + RefusalText(event.code, event.text) + ".", event.code != 403);
				break;

			case Type::FAILED:
				ConnectionLost(event.text);
				break;

			case Type::CLOSED:
				switch (event.code) {
				case 4001:
					StopNet("You were kicked from the game.", false);
					break;
				case 4002:
					StopNet("The game was deleted.", false);
					break;
				case 4003:
					StopNet("You joined this game from somewhere else, so this connection closed.", true);
					break;
				case 4004:
					StopNet("You were banned from the game.", false);
					break;
				case 1000:
				case 1008:
					StopNet("The server closed the connection" + (event.text.empty() ? std::string(".") : " (" + event.text + ")."), true);
					break;
				default:
					ConnectionLost(event.text.empty() ? "code " + std::to_string(event.code) : event.text);
					break;
				}
				break;
			}
		}

		// Leaves online play, printing why, and the server's board with it
		// keep_session lets bxt_bingo_join without a code go back to the game
		void StopNet(const std::string& reason, bool keep_session)
		{
			if (net.socket)
				net.socket->Close(1000);

			auto session_token = keep_session ? net.session_token : std::string();
			auto url = net.url;
			auto pending = keep_session ? net.pending : std::vector<PendingResult>();

			// A failed first join leaves the last game's session alone
			if (!keep_session && !net.session_token.empty())
				ClearSession();

			net = Net();
			net.session_token = session_token;
			net.url = url;
			net.pending = pending;
			ShowMessage(reason);

			// The board can't be played without the server, e.g. after a kick
			// Joining again brings it back
			my_team = Owner::NONE;
			team_colors = {};
			if (manifest.loaded)
				Leave();
		}
	}

	Net net;

	bool Online()
	{
		return net.state != NetState::OFFLINE;
	}

	// Only once in the game, messages before welcome would be refused
	void NetSend(const std::string& message)
	{
		if (net.state == NetState::JOINED && net.socket)
			net.socket->Send(message);
	}

	// A random UUID (version 4), to tell attempts apart
	std::string NewAttemptId()
	{
		static std::mt19937_64 rng(std::random_device{}() ^ static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
		uint64_t a = (rng() & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;
		uint64_t b = (rng() & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;

		char text[37];
		std::snprintf(text, sizeof(text), "%08x-%04x-%04x-%04x-%012llx",
			static_cast<unsigned>(a >> 32), static_cast<unsigned>(a >> 16 & 0xffff), static_cast<unsigned>(a & 0xffff),
			static_cast<unsigned>(b >> 48), static_cast<unsigned long long>(b & 0xffffffffffffULL));
		return text;
	}

	void ClearSession()
	{
		auto path = SessionPath();
		if (!path.empty())
			std::remove(path.c_str());
	}

	// The last game's session, for bxt_bingo_join without a code
	bool LoadSession()
	{
		std::string text;
		if (!ReadFile(SessionPath(), text))
			return false;

		rapidjson::Document doc;
		doc.Parse(text.c_str());
		if (doc.HasParseError() || !doc.IsObject())
			return false;

		std::string error;
		if (!GetString(doc, "url", net.url, error) || !GetString(doc, "session_token", net.session_token, error))
			return false;

		net.pending.clear();
		auto pending = doc.FindMember("pending");
		if (pending != doc.MemberEnd() && pending->value.IsArray()) {
			for (const auto& value : pending->value.GetArray()) {
				if (!value.IsObject())
					continue;

				PendingResult result;
				if (!GetString(value, "message", result.message, error))
					continue;

				rapidjson::Document message;
				message.Parse(result.message.c_str());
				if (message.HasParseError() || !message.IsObject() || !GetString(message, "attempt_id", result.attempt_id, error))
					continue;

				auto tile = value.FindMember("tile");
				auto time = value.FindMember("time_ms");
				result.tile = tile != value.MemberEnd() && tile->value.IsInt() ? tile->value.GetInt() : -1;
				result.time_ms = time != value.MemberEnd() && time->value.IsInt() ? time->value.GetInt() : 0;
				net.pending.push_back(std::move(result));
			}
		}

		return true;
	}

	// The tile the player picked (a click or Enter on the board, or bxt_bingo_play), -1 for none
	// From here the player contests it
	void SendTileSelected(int index)
	{
		NetSend(JsonMessage("tile_selected", [&](JsonWriter& w) {
			w.Key("tile");
			if (index >= 0)
				w.String(TileId(index).c_str());
			else
				w.Null();
		}));
	}

	// The start trigger fired: from here the server times the run on its own clock
	void SendAttemptStarted()
	{
		NetSend(JsonMessage("attempt_started", [&](JsonWriter& w) {
			w.Key("attempt_id");
			w.String(attempt.id.c_str());
			w.Key("tile");
			w.String(TileId(current_tile).c_str());
		}));
	}

	// The run broke a rule after it started, so it won't be sent as a result
	void SendAttemptInvalidated(const std::string& reason)
	{
		if (attempt.id.empty() || attempt.reported)
			return;

		attempt.reported = true;

		// The server takes 1 to 200 characters, without control characters
		std::string text;
		for (char c : reason) {
			if (static_cast<unsigned char>(c) >= 32 && c != 127 && text.size() < 200)
				text += c;
		}
		if (text.empty())
			text = "invalid";
		NetSend(JsonMessage("attempt_invalidated", [&](JsonWriter& w) {
			w.Key("attempt_id");
			w.String(attempt.id.c_str());
			w.Key("tile");
			w.String(TileId(current_tile).c_str());
			w.Key("reason");
			w.String(text.c_str());
		}));
	}

	// Kept until the server acknowledges it, and sent again after a reconnect
	// The other clocks come with evidence (BINGO.md §9 step 7): until then the server clock check
	// gets the real time, with everything but the game time counted as loading
	void SendAttemptResult(int time_ms, long long real_ms)
	{
		attempt.reported = true;
		real_ms = std::max<long long>(real_ms, time_ms);

		PendingResult result;
		result.attempt_id = attempt.id;
		result.tile = current_tile;
		result.time_ms = time_ms;
		result.message = JsonMessage("attempt_result", [&](JsonWriter& w) {
			w.Key("attempt_id");
			w.String(attempt.id.c_str());
			w.Key("tile");
			w.String(TileId(current_tile).c_str());
			w.Key("time_ms");
			w.Int(time_ms);
			w.Key("server_time_delta_ms");
			w.Int(time_ms);
			w.Key("frames");
			w.Int(0);
			w.Key("real_ms");
			w.Int64(real_ms);
			w.Key("load_ms");
			w.Int64(real_ms - time_ms);
			w.Key("save_sha256");
			w.String(manifest.tiles[current_tile].save_sha256.c_str());
			w.Key("ruleset_ok");
			w.Bool(true);
			w.Key("demo");
			w.Null();
		});

		net.pending.push_back(result);
		SaveSession();
		NetSend(result.message);
	}

	// A tile's new holder, the way a board update from the server brings it
	// Changing hands plays the ally or opponent capture, the player's own capture has its sound already
	void SetTileOwner(int index, Owner owner, int time_ms, const std::string& holder)
	{
		auto& tile = manifest.tiles[index];
		bool taken = owner != Owner::NONE && owner != tile.owner;
		tile.owner = owner;
		tile.time_ms = time_ms;
		tile.holder = holder;

		if (!taken || (owner == OfflineTeam() && holder == MyName()))
			return;

		// Online, the server's event message says what happened
		if (Online()) {
			PlayEventSound(owner == OfflineTeam() ? Event::ALLY_CAPTURE : Event::OPPONENT_CAPTURE);
			return;
		}

		auto who = holder.empty() ? "Team " + std::string(TeamName(owner)) : holder;
		auto time = time_ms >= 0 ? " in " + FormatTime(time_ms) : "";
		if (owner == OfflineTeam())
			Notify(Event::ALLY_CAPTURE, who + " took " + TileName(index) + " for your team" + time + ".");
		else
			Notify(Event::OPPONENT_CAPTURE, who + " took " + TileName(index) + " for team " + TeamName(owner) + time + ".");
	}

	// Who's running a tile, the way a board update from the server brings it
	// An opponent picking the tile you're playing plays the contested sound
	void SetContesting(int index, std::vector<Owner> teams)
	{
		auto me = OfflineTeam();
		auto opponents = [me](const std::vector<Owner>& list) {
			return std::count_if(list.begin(), list.end(), [me](Owner team) { return team != me; });
		};

		auto& tile = manifest.tiles[index];
		auto before = opponents(tile.contesting);
		auto after = opponents(teams);
		tile.contesting = std::move(teams);

		if (index == current_tile && IsAttemptActive() && after > before) {
			auto other = me == Owner::RED ? Owner::BLUE : Owner::RED;
			Notify(Event::CONTESTED, "Team " + std::string(TeamName(other)) + " is now also playing " + TileName(index) + ".");
		}
	}

	// bxt_bingo_server as a WebSocket address: ws:// or wss:// as given, or else a host
	// (with an optional port) that gets ws:// on the local network and wss:// otherwise
	// The path is /bxt unless one is given
	std::string ServerUrl(std::string server, std::string& error)
	{
		boost::algorithm::trim(server);
		if (server.empty()) {
			error = "Set the server first: bxt_bingo_server <address>, e.g. bxt_bingo_server \"localhost:8787\".";
			return {};
		}

		std::string scheme;
		std::string rest;
		for (auto prefix : { "wss://", "ws://", "https://", "http://" }) {
			if (boost::istarts_with(server, prefix)) {
				scheme = boost::istarts_with(prefix, "https") || boost::istarts_with(prefix, "wss") ? "wss://" : "ws://";
				rest = server.substr(std::strlen(prefix));
				break;
			}
		}

		if (scheme.empty()) {
			rest = server;
			auto host_end = rest[0] == '[' ? rest.find(']') + 1 : rest.find_first_of(":/");
			auto host = rest.substr(0, host_end == std::string::npos ? rest.size() : host_end);
			scheme = IsLocalHost(host) ? "ws://" : "wss://";
		}

		if (rest.empty() || rest[0] == '/') {
			error = "bxt_bingo_server needs a host, e.g. bingo.jrik.dev or \"localhost:8787\".";
			return {};
		}

		// No path, or only a slash
		auto slash = rest.find('/');
		if (slash == std::string::npos || slash == rest.size() - 1)
			rest = rest.substr(0, slash) + "/bxt";

		return scheme + rest;
	}

	void StartConnection()
	{
		std::vector<std::string> headers;
		if (!net.session_token.empty())
			headers.push_back("X-Bingo-Session: " + net.session_token);
		else
			headers.push_back("X-Bingo-Join: " + net.join_code);

		net.socket = Platform::OpenSocket(net.url, headers);
		net.opened = false;
		net.state = NetState::CONNECTING;
		net.last_heard = std::chrono::steady_clock::now();
	}

	// Called every frame: connects, reads what came in, pings
	void NetFrame()
	{
		if (!Online())
			return;

		// Downloads go on while reconnecting
		FilesFrame();

		auto now = std::chrono::steady_clock::now();
		if (net.state == NetState::RETRYING) {
			if (now >= net.retry_at)
				StartConnection();
			return;
		}

		Platform::SocketEvent event;
		while (net.socket && net.socket->Poll(event))
			OnSocketEvent(event);

		if (!net.socket || !net.opened)
			return;

		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - net.last_ping).count() >= PING_INTERVAL_MS) {
			net.last_ping = now;
			net.socket->Send("{\"type\":\"ping\"}");
		}

		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - net.last_heard).count() > SILENCE_LIMIT_MS)
			ConnectionLost("the server stopped answering");
	}
}
