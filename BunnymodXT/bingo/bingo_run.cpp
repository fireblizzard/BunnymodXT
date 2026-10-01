#include "../stdafx.hpp"

#include <boost/algorithm/string/predicate.hpp>

#include "../modules/ClientDLL.hpp"
#include "../modules/HwDLL.hpp"
#include "../git_revision.hpp"
#include "../hud_custom.hpp"
#include "bingo_internal.hpp"

namespace Bingo
{
	namespace
	{
		xcommand_t original_load = nullptr;
		xcommand_t original_save = nullptr;
		xcommand_t original_autosave = nullptr;
		xcommand_t original_map = nullptr;
		xcommand_t original_changelevel = nullptr;
		xcommand_t original_reload = nullptr;
		xcommand_t original_restart = nullptr;

		// The player's health and armor on the last frame, to see damage for the no_damage handicap
		struct Vitals
		{
			bool known = false;
			float health = 0;
			float armor = 0;
		};

		Vitals vitals;

		// Waiting for a save to load gives up after this
		constexpr long long MAX_LOAD_MS = 10000;

		// Saves made during the current run, which segmented runs may load
		// The hash is taken right after saving, so a save swapped from outside the game is caught on load
		std::unordered_map<std::string, FileHash> run_saves;

		// The last save made since a tile was picked, which is the one reload loads
		std::string newest_save;

		bool IsCurrentMap(const std::string& map)
		{
			auto& cl = ClientDLL::GetInstance();
			if (!cl.pEngfuncs)
				return false;

			char map_name[64];
			cl.GetMapName(map_name, sizeof(map_name));
			return boost::iequals(map_name, map);
		}

		// BXT's timer (bxt_hud_timer, and LiveSplit through bxt_interprocess_enable) follows the run
		// Through the timer itself rather than the bxt_timer_* commands, which also do things
		// for full-game runs (resetting splits, marking demos)
		void ResetTimer()
		{
			CustomHud::ResetTime();
		}

		void StartTimer()
		{
			CustomHud::SetCountingTime(true);
		}

		void StopTimer()
		{
			CustomHud::SetCountingTime(false);
		}

		// The run keeps going like BXT's prevented hornet crash (red timer, INVALID), but won't count
		void InvalidateAttempt(const std::string& reason)
		{
			if (attempt.state != AttemptState::RUNNING || !attempt.invalid_reason.empty())
				return;

			attempt.invalid_reason = reason;
			SendAttemptInvalidated(reason);
			CustomHud::SetInvalidRun(true);
			Notify(Event::INVALID, "Run of " + TileName(current_tile) + " no longer counts: " + reason + ". You can keep playing it.");
		}

		// Any drop in health or armor while running, for the no_damage handicap
		// Starts over after loads, as a loaded save can have less health without any damage
		void CheckDamage()
		{
			auto pl = HwDLL::GetInstance().GetPlayerEdict();
			if (!manifest.ruleset.no_damage || attempt.state != AttemptState::RUNNING
				|| HwDLL::GetInstance().GetClientState() != ca_active || !pl) {
				vitals.known = false;
				return;
			}

			if (vitals.known && (pl->v.health < vitals.health || pl->v.armorvalue < vitals.armor)) {
				char reason[128];
				std::snprintf(reason, sizeof(reason), "took damage (health %.0f to %.0f, armor %.0f to %.0f)",
					vitals.health, pl->v.health, vitals.armor, pl->v.armorvalue);
				InvalidateAttempt(reason);
			}

			vitals = { true, pl->v.health, pl->v.armorvalue };
		}

		// Single-segment runs end when the player dies
		void CheckDeath()
		{
			auto pl = HwDLL::GetInstance().GetPlayerEdict();
			if (manifest.ruleset.single_segment && attempt.state == AttemptState::RUNNING
				&& HwDLL::GetInstance().GetClientState() == ca_active && pl && pl->v.health <= 0)
				CancelAttempt("died in a single-segment run");
		}

		// Lets go of blocked +commands the player was already holding when the run starts
		// (a - command with no key releases the button from every key)
		void ReleaseBlockedButtons()
		{
			for (const auto& command : manifest.ruleset.blocked_commands) {
				if (command.size() < 2 || command[0] != '+' || command.find_first_of(" *") != std::string::npos)
					continue;

				auto release = "-" + command.substr(1) + "\n";
				ClientDLL::GetInstance().pEngfuncs->pfnClientCmd(const_cast<char*>(release.c_str()));
			}
		}

		// Cancels the run if a cvar breaks the rules
		void CheckRules()
		{
			if (attempt.state != AttemptState::ARMED && attempt.state != AttemptState::RUNNING)
				return;

			auto violation = Rules::CheckCvars();
			if (!violation.empty())
				CancelAttempt(violation.c_str());
		}

		// The timer starts. When online the server starts timing the run too
		void BeginRun()
		{
			auto& hw = HwDLL::GetInstance();
			attempt.state = AttemptState::RUNNING;
			attempt.started_at = std::chrono::steady_clock::now();
			attempt.reported = false;
			attempt.kills = 0;
			attempt.frames = 0;
			attempt.stopped_time = 0;
			attempt.server_time = 0;
			attempt.last_server_time = hw.IsActive() ? hw.GetTime() : -1;
			attempt.last_frame = attempt.started_at;
			StartTimer();

			if (Online()) {
				if (attempt.id.empty())
					attempt.id = NewAttemptId();
				SendAttemptStarted();

				const auto& tile = manifest.tiles[current_tile];
				AddDemoInfo("start", [&](JsonWriter& w) {
					w.Key("game_id");
					w.String(net.game_id.c_str());
					w.Key("server");
					w.String(net.url.c_str());
					w.Key("steamid64");
					w.String(net.steamid64.c_str());
					w.Key("name");
					w.String(net.name.c_str());
					w.Key("team");
					w.String(TeamName(my_team));
					w.Key("tile");
					w.String(TileId(current_tile).c_str());
					w.Key("label");
					w.String(tile.label.c_str());
					w.Key("save_sha256");
					w.String(tile.save_sha256.c_str());
					w.Key("manifest_hash");
					w.String(manifest.hash.c_str());
					w.Key("run_type");
					w.String(RunType().c_str());
					w.Key("handicaps");
					w.String(Handicaps().c_str());
					w.Key("match_clock_ms");
					w.Int64(MatchClockMs());
					w.Key("bxt_version");
					w.String(Git::GetRevision());
					w.Key("bxt_dll_sha256");
					w.String(DllSha256().c_str());
				});
			}
		}

		// Time the game doesn't run, in loading screens or paused, is counted apart
		void CountClocks()
		{
			if (attempt.state != AttemptState::RUNNING)
				return;

			auto& hw = HwDLL::GetInstance();
			auto now = std::chrono::steady_clock::now();
			double elapsed = std::chrono::duration<double>(now - attempt.last_frame).count();
			attempt.last_frame = now;

			if (hw.GetClientState() != ca_active || !hw.IsActive() || hw.IsPaused()) {
				attempt.stopped_time += elapsed;
				attempt.last_server_time = -1;
				return;
			}

			++attempt.frames;

			// A load sets the server's time to the save's, which isn't time that went by
			double time = hw.GetTime();
			if (attempt.last_server_time >= 0 && time > attempt.last_server_time && time - attempt.last_server_time < 1)
				attempt.server_time += time - attempt.last_server_time;
			attempt.last_server_time = time;
		}

		// The save loaded, so the tile's triggers go live
		void ArmAttempt()
		{
			Rules::Arm(manifest.ruleset);
			ReleaseBlockedButtons();

			const auto& tile = manifest.tiles[current_tile];
			if (!tile.end_on_game_end)
				end_trigger.emplace(tile.end, true);
			if (tile.start_on_load) {
				start_trigger.reset();
				BeginRun();
			} else {
				start_trigger.emplace(tile.start, false);
				attempt.state = AttemptState::ARMED;
			}
		}

		void UpdateAttempt()
		{
			if (attempt.state != AttemptState::LOADING)
				return;

			if (HwDLL::GetInstance().GetClientState() != ca_active) {
				attempt.saw_loading = true;
				return;
			}

			if (attempt.saw_loading && IsInMap()) {
				ArmAttempt();
				return;
			}

			auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - attempt.load_started).count();
			if (ms > MAX_LOAD_MS)
				CancelAttempt("the save didn't load");
		}

		void OnStartTrigger()
		{
			// Touching the start trigger again during a run changes nothing, or a player could shorten
			// their time by walking back into it
			if (attempt.state != AttemptState::ARMED)
				return;

			BeginRun();
		}

		// Offline: the first finish takes the tile, a strictly faster time takes it over (ties keep
		// the first time), with the team from _bxt_bingo_set_team or red
		// With a server, the server decides this
		void OnEndTrigger()
		{
			if (attempt.state != AttemptState::RUNNING)
				return;

			CountClocks();
			attempt.state = AttemptState::FINISHED;
			StopTimer();

			int time_ms = TimerMs();

			// The demo of a run with a time stays, whatever the time does
			AddDemoInfo("finish", [&](JsonWriter& w) {
				w.Key("time_ms");
				w.Int(time_ms);
				w.Key("server_time_ms");
				w.Int64(std::llround(attempt.server_time * 1000));
				w.Key("frames");
				w.Int(attempt.frames);
				w.Key("real_ms");
				w.Int64(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - attempt.started_at).count());
				w.Key("stopped_ms");
				w.Int64(std::llround(attempt.stopped_time * 1000));
				w.Key("invalid_reason");
				w.String(attempt.invalid_reason.c_str());
			});
			EndDemo(true);
			auto& tile = manifest.tiles[current_tile];
			auto team = OfflineTeam();

			char map_name[64] = {};
			ClientDLL::GetInstance().GetMapName(map_name, sizeof(map_name));

			// With the Bloodthirsty handicap a run without a kill doesn't count, have to make at least 1 enemy kill
			if (manifest.ruleset.require_kill && attempt.kills == 0 && attempt.invalid_reason.empty()) {
				attempt.invalid_reason = "no monster was killed (Bloodthirsty)";
				SendAttemptInvalidated(attempt.invalid_reason);
				Notify(Event::INVALID, "Finished " + TileName(current_tile) + " in " + FormatTime(time_ms) + ", but it doesn't count: " + attempt.invalid_reason + ".");
				return;
			}

			if (!attempt.invalid_reason.empty()) {
				Print("Finished %s in %s (on %s), but it doesn't count: %s.\n", TileName(current_tile).c_str(), FormatTime(time_ms).c_str(), map_name, attempt.invalid_reason.c_str());
				return;
			}

			// Online, so the server decides what the time does
			if (Online() && !attempt.id.empty()) {
				auto real_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - attempt.started_at).count();
				Print("Finished %s in %s (on %s), sending it to the server.\n", TileName(current_tile).c_str(), FormatTime(time_ms).c_str(), map_name);
				SendAttemptResult(std::max(1, time_ms), real_ms);
				return;
			}

			const char* result;
			if (tile.owner == Owner::NONE)
				result = "captured";
			else if (time_ms >= tile.time_ms) {
				Print("Finished %s in %s (on %s): not faster than %s.\n", TileName(current_tile).c_str(), FormatTime(time_ms).c_str(), map_name, FormatTime(tile.time_ms).c_str());
				return;
			} else if (tile.owner == team)
				result = "new best time";
			else
				result = "taken over";

			auto engfuncs = ClientDLL::GetInstance().pEngfuncs;
			auto name = engfuncs->pfnGetCvarString(const_cast<char*>("name"));

			tile.owner = team;
			tile.time_ms = time_ms;
			tile.holder = name ? name : "";
			Notify(Event::CAPTURE, "Finished " + TileName(current_tile) + " in " + FormatTime(time_ms) + " (on " + map_name + "): " + result + " for team " + TeamName(team) + ".");
		}

		// Loading the retry save starts the tile over
		void RestartAttempt()
		{
			// Online, a tile the team can't play any more (e.g. it just took it) doesn't arm,
			// or the server would refuse the run
			if (Online() && !manifest.tiles[current_tile].playable) {
				CancelAttempt("your team can't play the tile now", false);
				attempt.state = AttemptState::IDLE;
				start_trigger.reset();
				end_trigger.reset();
				Print("%s can't be played by your team now, pick another tile.\n", TileName(current_tile).c_str());
				return;
			}

			// Only the tile's own save arms a run
			// If the retry save changed (e.g. saved over after a run), put the tile's save back first
			auto retry_save = RetrySaveName();
			const auto& tile = manifest.tiles[current_tile];
			auto path = SavePath(retry_save.c_str());
			if (!IsTileSave(path, tile)) {
				auto pristine = SavePath(TileSaveName(tile).c_str());
				if (CopyFile(pristine, path) && IsTileSave(path, tile))
					Print("%s.sav had changed, loading the save for %s instead.\n", retry_save.c_str(), TileName(current_tile).c_str());
				else {
					CancelAttempt("the retry save doesn't match the tile and couldn't be put back");
					return;
				}
			}

			// The last try's demo goes unless it reached the end
			if (attempt.state != AttemptState::FINISHED)
				EndDemo(false);

			attempt.state = AttemptState::LOADING;
			ResetTimer();
			attempt.saw_loading = false;
			attempt.load_started = std::chrono::steady_clock::now();
			start_trigger.reset();
			end_trigger.reset();
			run_saves.clear();
			attempt.invalid_reason.clear();
			attempt.id.clear();
			attempt.demo.clear();
			attempt.nonce.clear();
			attempt.reported = false;

			// Online, the attempt is recorded from this load on
			if (Online()) {
				attempt.id = NewAttemptId();
				StartDemo();
			}

			// Saves from the last try don't belong to this one
			newest_save = retry_save;
		}

		// Runs before the engine loads a save
		// Segmented runs keep going (and the timer keeps running) when the save was made during the run
		void BeforeLoad(const std::string& name)
		{
			if (current_tile < 0)
				return;

			if (name == RetrySaveName()) {
				RestartAttempt();
				return;
			}

			if (!IsAttemptActive())
				return;

			if (manifest.ruleset.single_segment) {
				CancelAttempt("loaded a save in a single-segment run");
				return;
			}

			auto it = run_saves.find(name);
			if (it == run_saves.end()) {
				auto reason = "loaded " + name + ".sav, which wasn't saved during this run";
				CancelAttempt(reason.c_str());
			} else if (!(HashSave(name) == it->second)) {
				auto reason = name + ".sav changed after it was saved during this run";
				CancelAttempt(reason.c_str());
			}
		}

		void WrappedLoad()
		{
			SetMapEnding(true);
			BeforeLoad(SaveNameArg());
			original_load();
			SetMapEnding(false);
		}

		// Hashes before and after the engine saves, as a failed save (e.g. while dead) leaves the old file
		// and a pre-run save must not count as made during the run
		void RememberSave(const std::string& name, const FileHash& before)
		{
			if (name.empty())
				return;

			auto after = HashSave(name);
			if (after.sha256.empty() || after == before) {
				EngineDevMsg("[bingo] %s.sav didn't change, not counting it as saved.\n", name.c_str());
				return;
			}

			newest_save = name;
			if (IsAttemptActive()) {
				run_saves[name] = after;
				EngineDevMsg("[bingo] %s.sav was saved during the run.\n", name.c_str());
			}
		}

		void WrappedSave()
		{
			auto name = SaveNameArg();

			// Loading the retry save restarts the tile, so saving over it would lose this save
			if (IsAttemptActive() && name == RetrySaveName()) {
				Print("%s.sav restarts %s, save under another name.\n", name.c_str(), TileName(current_tile).c_str());
				return;
			}

			auto before = HashSave(name);
			original_save();
			RememberSave(name, before);
		}

		// Maps call this through trigger_autosave
		void WrappedAutosave()
		{
			auto before = HashSave("autosave");
			original_autosave();
			RememberSave("autosave", before);
		}

		// Commands typed during a run that would skip part of the segment
		void WrappedMap()
		{
			SetMapEnding(true);
			CancelAttempt("used map");
			original_map();
			SetMapEnding(false);
		}

		void WrappedChangelevel()
		{
			CancelAttempt("used changelevel");
			original_changelevel();
		}

		// `reload` (e.g. after dying) loads the newest save, or restarts the map if there's none
		// While a tile is picked, bingo picks the save instead of the engine, which may load an older
		// autosave or quicksave: the newest one made during the run, or the tile's start
		// Single-segment runs, and runs that already ended, start over from the tile's start
		void WrappedReload()
		{
			if (current_tile < 0) {
				original_reload();
				return;
			}

			auto save = newest_save;
			if (save.empty() || manifest.ruleset.single_segment || !IsAttemptActive())
				save = RetrySaveName();

			// Through load, which checks the save like when the player loads it
			auto command = "load " + save + "\n";
			ClientDLL::GetInstance().pEngfuncs->pfnClientCmd(const_cast<char*>(command.c_str()));
		}

		void WrappedRestart()
		{
			SetMapEnding(true);
			CancelAttempt("used restart");
			original_restart();
			SetMapEnding(false);
		}
	}

	Attempt attempt;

	// The current tile's triggers, separate from bxt_triggers_* so the player's own can't touch a run
	std::optional<TileTrigger> start_trigger;
	std::optional<TileTrigger> end_trigger;

	bool IsAttemptActive()
	{
		return attempt.state == AttemptState::LOADING
			|| attempt.state == AttemptState::ARMED
			|| attempt.state == AttemptState::RUNNING;
	}

	// The run's time, which is BXT's timer
	// bingo is the only thing that starts and stops it during a run, as the ruleset
	// keeps bxt_timer_* and the split cvars that control it away from the player
	int TimerMs()
	{
		auto time = CustomHud::GetTime();
		return ((static_cast<int>(time.hours) * 60 + time.minutes) * 60 + time.seconds) * 1000 + time.milliseconds;
	}

	// The sound and message are for runs the rules ended, not for the player moving on
	// (picking another tile, leaving)
	void CancelAttempt(const char* reason, bool notify)
	{
		if (!IsAttemptActive())
			return;

		// A run that started is reported to the server, for its stats
		if (attempt.state == AttemptState::RUNNING)
			SendAttemptInvalidated(reason);

		attempt.state = AttemptState::IDLE;
		StopTimer();
		EndDemo(false);

		auto text = "Run of " + TileName(current_tile) + " cancelled: " + reason + ".";
		if (notify)
			Notify(Event::INVALID, text);
		else
			Print("%s\n", text.c_str());
	}

	// "scriptless, segmented" and the like
	std::string RunType()
	{
		std::string type = manifest.ruleset.scripted ? "scripted" : "scriptless";
		return type + (manifest.ruleset.single_segment ? ", single-segment" : ", segmented");
	}

	// "no damage, blocked +attack2", or empty without handicaps
	// Single-segment is in RunType
	std::string Handicaps()
	{
		std::string text = manifest.ruleset.no_damage ? "no damage" : "";
		if (!manifest.ruleset.blocked_commands.empty()) {
			text += text.empty() ? "blocked" : ", blocked";
			for (const auto& command : manifest.ruleset.blocked_commands)
				text += " " + command;
		}

		if (manifest.ruleset.require_kill)
			text += text.empty() ? "a kill needed" : ", a kill needed";

		for (const auto& rule : manifest.ruleset.cvars) {
			if (rule.op == Rules::CvarOp::SET)
				text += (text.empty() ? "" : ", ") + rule.name + " " + rule.value;
		}

		return text;
	}

	bool TileTrigger::counts_here() const
	{
		return map.empty() || IsCurrentMap(map);
	}

	void TileTrigger::touch()
	{
		if (!counts_here())
			return;

		if (is_end)
			OnEndTrigger();
		else
			OnStartTrigger();
	}

	// Copies the tile's save to the retry save and loads it, which arms the attempt
	void PickTile(int index)
	{
		if (index < 0)
			return;

		const auto& tile = manifest.tiles[index];
		if (Online() && net.lobby_state != "running") {
			Print("The game hasn't started yet.\n");
			return;
		}

		if (!tile.playable) {
			Print("%s can't be played by your team right now.\n", TileName(index).c_str());
			return;
		}

		auto pristine = SavePath(TileSaveName(tile).c_str());
		if (pristine.empty())
			return;

		if (!FileExists(pristine)) {
			Print("The save for %s is missing: %s.\n", TileName(index).c_str(), pristine.c_str());
			return;
		}

		if (!IsTileSave(pristine, tile)) {
			Print("The save for %s doesn't match the board: %s.\n", TileName(index).c_str(), pristine.c_str());
			return;
		}

		auto retry_save = RetrySaveName();
		BackUpRetrySave(retry_save);
		if (!CopyFile(pristine, SavePath(retry_save.c_str()))) {
			Print("Could not copy the save for %s to %s.sav.\n", TileName(index).c_str(), retry_save.c_str());
			return;
		}

		if (index != current_tile)
			CancelAttempt("picked another tile", false);

		current_tile = index;
		CloseBoard(true);
		SendTileSelected(index);

		newest_save = retry_save;

		Print("Playing %s (%s), retry with `load %s`.\n", TileName(index).c_str(), RunType().c_str(), retry_save.c_str());
		auto handicaps = Handicaps();
		if (!handicaps.empty())
			Print("Your handicaps: %s.\n", handicaps.c_str());
		auto command = "load " + retry_save + "\n";
		ClientDLL::GetInstance().pEngfuncs->pfnClientCmd(const_cast<char*>(command.c_str()));
	}

	// Called once from Init()
	void WrapRunCommands()
	{
		WrapCommand("load", original_load, WrappedLoad);
		WrapCommand("save", original_save, WrappedSave);
		WrapCommand("autosave", original_autosave, WrappedAutosave);
		WrapCommand("map", original_map, WrappedMap);
		WrapCommand("changelevel", original_changelevel, WrappedChangelevel);
		WrapCommand("reload", original_reload, WrappedReload);
		WrapCommand("restart", original_restart, WrappedRestart);
	}

	// Called every frame from Frame()
	// Follows the attempt's loading, and checks the rules and handicaps
	void RunFrame()
	{
		CountClocks();
		UpdateAttempt();
		// Before the rules check them
		static const Rules::Ruleset no_rules;
		Rules::ApplySetCvars(manifest.loaded ? manifest.ruleset : no_rules);

		CheckRules();
		CheckDamage();
		CheckDeath();
	}

	void UpdateTriggers(const Vector& player_position, bool ducking)
	{
		if (start_trigger)
			start_trigger->update(player_position, ducking);
		if (end_trigger)
			end_trigger->update(player_position, ducking);
	}

	void UpdateTriggers(const Vector& player_position_start, const Vector& player_position_end, bool ducking)
	{
		if (start_trigger)
			start_trigger->update(player_position_start, player_position_end, ducking);
		if (end_trigger)
			end_trigger->update(player_position_start, player_position_end, ducking);
	}

	void OnGameEnd()
	{
		// Segments like Nihilanth end when the game does
		if (current_tile >= 0 && manifest.tiles[current_tile].end_on_game_end)
			OnEndTrigger();
	}

	void OnMonsterKilled(const entvars_t* monster, const char* classname, const entvars_t* attacker)
	{
		// Allies and harmless critters aren't enemies
		static const char* const NOT_ENEMIES[] = {
			"monster_barney",
			"monster_scientist",
			"monster_sitting_scientist",
			"monster_cockroach",
			"monster_rat",
			"monster_bloater",
			"monster_flyer",
		};

		if (!monster || !classname || attempt.state != AttemptState::RUNNING)
			return;

		// Blowing up a dead body would call this again and with one kill it's enough for the Bloodthirsty handicap
		if (monster->deadflag != DEAD_NO)
			return;

		// The player's own attack, including their grenades and other explosives
		if (!attacker || !(attacker->flags & FL_CLIENT))
			return;

		if (std::any_of(std::begin(NOT_ENEMIES), std::end(NOT_ENEMIES), [&](const char* name) { return !std::strcmp(classname, name); }))
			return;

		if (++attempt.kills == 1 && manifest.ruleset.require_kill)
			ShowMessage(std::string("Killed a ") + classname + ", so this run can count (Bloodthirsty).");
	}

	bool OnPlayerCommand(const char* text)
	{
		if (!IsAttemptActive())
			return true;

		// Blocked from loading the tile on, so a button can't be pressed just before the start
		auto blocked = Rules::FindBlockedCommand(manifest.ruleset, text);
		if (!blocked.empty()) {
			Print("%s is blocked for you in this game.\n", blocked.c_str());
			return false;
		}

		if (attempt.state != AttemptState::ARMED && attempt.state != AttemptState::RUNNING)
			return true;

		auto violation = Rules::CheckPlayerCommand(manifest.ruleset, text);
		if (!violation.empty())
			CancelAttempt(violation.c_str());

		return true;
	}
}
