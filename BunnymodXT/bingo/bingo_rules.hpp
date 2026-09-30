#pragma once

#include <rapidjson/document.h>

// The rules a bingo run has to follow, from the manifest's ruleset
// Same shape as Ruleset in the server's protocol (src/protocol/segment.js in bxt-bingo-server)
namespace Bingo
{
	namespace Rules
	{
		// How the cvar value given by the ruleset will be compared against the actual cvar value the player has
		enum class CvarOp
		{
			EQ,
			NE,
			LTE,
			GTE,
			SET,       // like EQ, and BXT sets it while the board is loaded (only sv_ cvars)
			DEFAULT,   // must stay at BXT's default, only for BXT's own cvars
			ANY,       // exempts a cvar from a wider * rule
			UNCHANGED  // must keep the value it had when the run started
		};

		struct CvarRule
		{
			std::string name; // a trailing * matches a prefix
			CvarOp op = CvarOp::EQ;
			std::string value;
		};

		struct Ruleset
		{
			bool single_segment = false;
			bool scripted = false;
			std::vector<CvarRule> cvars;

			// A trailing * allows a prefix, and "name arg" allows the command only with that first argument
			// Empty allows every command
			std::vector<std::string> allowed_commands;

			// Commands a scripted run may only run on their own
			std::vector<std::string> not_in_scripts;

			// Commands that are dropped instead of cancelling the run (handicaps like "No +attack2")
			std::vector<std::string> blocked_commands;

			// Taking damage after the start trigger invalidates the run (the "No damage%" handicap)
			bool no_damage = false;

			// The run only counts after killing an enemy monster (the "Bloodthirsty" handicap)
			bool require_kill = false;
		};

		// Reads the manifest's "ruleset" object, where everything is optional
		bool Parse(const rapidjson::Value& value, Ruleset& ruleset, std::string& error);

		// Call when a run is armed, to find the cvars the rules cover
		// and remember the values that must stay unchanged
		void Arm(const Ruleset& ruleset);

		// Returns why the cvars break the rules, or an empty string
		std::string CheckCvars();

		// Call every frame, before CheckCvars: sets the cvars of the ruleset's set rules,
		// and puts back the player's own values of the ones it no longer sets
		// An empty ruleset puts them all back
		void ApplySetCvars(const Ruleset& ruleset);

		// Checks text the player ran from a key or the console
		// Returns why it breaks the rules, or an empty string
		std::string CheckPlayerCommand(const Ruleset& ruleset, const std::string& text);

		// The first blocked command the text runs, or an empty string
		std::string FindBlockedCommand(const Ruleset& ruleset, const std::string& text);
	}
}
