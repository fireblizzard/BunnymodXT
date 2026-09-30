#include "../stdafx.hpp"

#include <limits>
#include <locale>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include "../modules/ClientDLL.hpp"
#include "../modules/HwDLL.hpp"
#include "../modules/ServerDLL.hpp"
#include "../cvars.hpp"
#include "bingo_rules.hpp"

namespace Bingo
{
	namespace Rules
	{
		namespace
		{
			// A cvar the rules cover, found when the run is armed
			struct CvarCheck
			{
				cvar_t* cvar;
				CvarOp op;
				std::string value;
			};

			std::vector<CvarCheck> cvar_checks;

			// The player's own values of the cvars set rules changed, lowercase name -> value
			std::unordered_map<std::string, std::string> set_cvar_originals;

			// Aliases and exec inside aliases and configs, deeper than this is treated as one command
			constexpr int MAX_EXPANSION_DEPTH = 8;

			bool ParseCvarOp(const std::string& text, CvarOp& op)
			{
				static const std::pair<const char*, CvarOp> OPS[] = {
					{ "eq", CvarOp::EQ },
					{ "ne", CvarOp::NE },
					{ "lte", CvarOp::LTE },
					{ "gte", CvarOp::GTE },
					{ "set", CvarOp::SET },
					{ "default", CvarOp::DEFAULT },
					{ "any", CvarOp::ANY },
					{ "unchanged", CvarOp::UNCHANGED },
				};

				for (const auto& pair : OPS) {
					if (text == pair.first) {
						op = pair.second;
						return true;
					}
				}

				return false;
			}

			bool ParseStringList(const rapidjson::Value& object, const char* name, std::vector<std::string>& out, std::string& error)
			{
				auto it = object.FindMember(name);
				if (it == object.MemberEnd())
					return true;

				if (!it->value.IsArray()) {
					error = std::string("\"") + name + "\" must be a list";
					return false;
				}

				for (const auto& item : it->value.GetArray()) {
					if (!item.IsString()) {
						error = std::string("\"") + name + "\" must only contain strings";
						return false;
					}

					out.emplace_back(item.GetString(), item.GetStringLength());
				}

				return true;
			}

			bool ParseBool(const rapidjson::Value& object, const char* name, bool& out, std::string& error)
			{
				auto it = object.FindMember(name);
				if (it == object.MemberEnd())
					return true;

				if (!it->value.IsBool()) {
					error = std::string("\"") + name + "\" must be true or false";
					return false;
				}

				out = it->value.GetBool();
				return true;
			}

			// Whether a rule's name covers the name, and how closely (longer is closer)
			// An exact name beats any prefix
			int MatchLength(const std::string& pattern, const char* name)
			{
				if (!pattern.empty() && pattern.back() == '*') {
					auto prefix = pattern.substr(0, pattern.size() - 1);
					return boost::istarts_with(name, prefix) ? static_cast<int>(prefix.size()) : -1;
				}

				return boost::iequals(pattern, name) ? std::numeric_limits<int>::max() : -1;
			}

			const CvarRule* FindCvarRule(const Ruleset& ruleset, const char* name)
			{
				const CvarRule* best = nullptr;
				int best_length = -1;
				for (const auto& rule : ruleset.cvars) {
					auto length = MatchLength(rule.name, name);
					if (length > best_length) {
						best = &rule;
						best_length = length;
					}
				}

				return best;
			}

			// BXT's default for one of its own cvars, or nullptr
			const char* BxtDefault(const char* name)
			{
				for (auto wrapper : CVars::allCVars) {
					auto cvar = wrapper->GetPointer();
					if (cvar && cvar->name && boost::iequals(cvar->name, name))
						return wrapper->GetDefault();
				}

				return nullptr;
			}

			// Always with a . for decimals, as strtod follows the system's locale,
			// where a comma can be the decimal mark and "200.0" would stop at the .
			bool ParseNumber(const std::string& text, double& out)
			{
				std::istringstream ss(text);
				ss.imbue(std::locale::classic());
				ss >> out;
				if (ss.fail())
					return false;

				ss >> std::ws;
				return ss.eof();
			}

			// An empty value counts as 0 like the engine reads it, as the sheet uses 0 for
			// text cvars that are off when empty (e.g. bxt_fire_on_button_command)
			bool ParseCvarNumber(const std::string& text, double& out)
			{
				if (text.empty()) {
					out = 0;
					return true;
				}

				return ParseNumber(text, out);
			}

			// Numbers compare as numbers, anything else as text
			int Compare(const std::string& a, const std::string& b)
			{
				double x, y;
				if (ParseCvarNumber(a, x) && ParseCvarNumber(b, y))
					return (x > y) - (x < y);

				return a.compare(b);
			}

			std::string Violation(const CvarCheck& check, const std::string& current)
			{
				std::string name = check.cvar->name;
				switch (check.op) {
				case CvarOp::EQ:
				case CvarOp::SET:
				case CvarOp::DEFAULT:
					return name + " must be " + check.value + " (it's " + current + ")";
				case CvarOp::NE:
					return name + " can't be " + check.value;
				case CvarOp::LTE:
					return name + " must be at most " + check.value + " (it's " + current + ")";
				case CvarOp::GTE:
					return name + " must be at least " + check.value + " (it's " + current + ")";
				case CvarOp::UNCHANGED:
					return name + " can't change during a run (it was " + check.value + ", now it's " + current + ")";
				case CvarOp::ANY:
					break;
				}

				return {};
			}

			bool Breaks(const CvarCheck& check, const std::string& current)
			{
				switch (check.op) {
				case CvarOp::EQ:
				case CvarOp::SET:
				case CvarOp::DEFAULT:
				case CvarOp::UNCHANGED:
					return Compare(current, check.value) != 0;
				case CvarOp::NE:
					return Compare(current, check.value) == 0;
				case CvarOp::LTE:
					return Compare(current, check.value) > 0;
				case CvarOp::GTE:
					return Compare(current, check.value) < 0;
				case CvarOp::ANY:
					break;
				}

				return false;
			}

			// Splits a command line like the engine does. `;` and newlines end a command outside quotes,
			// and `//` starts a comment until the end of the line
			std::vector<std::string> SplitCommands(const std::string& text)
			{
				std::vector<std::string> commands;
				std::string current;
				bool quoted = false;
				bool comment = false;

				auto finish = [&]() {
					auto start = current.find_first_not_of(" \t\r");
					if (start != std::string::npos)
						commands.push_back(current.substr(start));
					current.clear();
				};

				for (size_t i = 0; i < text.size(); ++i) {
					char c = text[i];
					if (c == '\n') {
						comment = false;
						quoted = false;
						finish();
						continue;
					}

					if (comment)
						continue;

					if (c == '"')
						quoted = !quoted;
					else if (!quoted && c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
						comment = true;
						continue;
					} else if (!quoted && c == ';') {
						finish();
						continue;
					}

					current += c;
				}

				finish();
				return commands;
			}

			// The command's words, with quotes removed
			std::vector<std::string> Words(const std::string& command)
			{
				std::vector<std::string> words;
				size_t i = 0;
				while (i < command.size()) {
					while (i < command.size() && std::isspace(static_cast<unsigned char>(command[i])))
						++i;
					if (i >= command.size())
						break;

					std::string word;
					if (command[i] == '"') {
						auto end = command.find('"', i + 1);
						if (end == std::string::npos)
							end = command.size();
						word = command.substr(i + 1, end - i - 1);
						i = end + 1;
					} else {
						while (i < command.size() && !std::isspace(static_cast<unsigned char>(command[i])))
							word += command[i++];
					}

					words.push_back(word);
				}

				return words;
			}

			const char* FindAlias(const std::string& name)
			{
				// Walked the same way as in HwDLL's Cmd_TokenizeString hook
				for (auto alias = HwDLL::GetInstance().GetAliases(); alias; alias = alias->next) {
					if (boost::iequals(alias->name, name))
						return alias->value;
				}

				return nullptr;
			}

			// The text of a config that exec would run, looked up like the engine does:
			// the game directory, then valve
			bool ReadConfig(const std::string& name, std::string& text)
			{
				auto& cl = ClientDLL::GetInstance();
				if (!cl.pEngfuncs)
					return false;

				std::string game_dir = cl.pEngfuncs->pfnGetGameDirectory();
				auto slash = game_dir.find_last_of("/\\");
				auto valve_dir = (slash == std::string::npos ? std::string() : game_dir.substr(0, slash + 1)) + "valve";

				for (const auto& dir : { game_dir, valve_dir }) {
					std::ifstream file(dir + "/" + name, std::ios::binary);
					if (file) {
						std::ostringstream ss;
						ss << file.rdbuf();
						text = ss.str();
						return true;
					}
				}

				return false;
			}

			// The commands the text runs, with aliases and configs replaced by what they run
			void Expand(const std::string& text, std::vector<std::string>& out, int depth)
			{
				for (const auto& command : SplitCommands(text)) {
					auto words = Words(command);
					if (words.empty())
						continue;

					if (depth < MAX_EXPANSION_DEPTH) {
						if (auto value = FindAlias(words[0])) {
							Expand(value, out, depth + 1);
							continue;
						}

						std::string config;
						if (boost::iequals(words[0], "exec") && words.size() >= 2 && ReadConfig(words[1], config)) {
							Expand(config, out, depth + 1);
							continue;
						}
					}

					out.push_back(command);
				}
			}

			bool MatchesCommand(const std::string& entry, const std::vector<std::string>& words)
			{
				if (!entry.empty() && entry.back() == '*')
					return boost::istarts_with(words[0], entry.substr(0, entry.size() - 1));

				auto space = entry.find(' ');
				if (space == std::string::npos)
					return boost::iequals(words[0], entry);

				return words.size() >= 2
					&& boost::iequals(words[0], entry.substr(0, space))
					&& boost::iequals(words[1], entry.substr(space + 1));
			}

			bool IsInList(const std::vector<std::string>& list, const std::vector<std::string>& words)
			{
				return std::any_of(list.begin(), list.end(), [&](const std::string& entry) { return MatchesCommand(entry, words); });
			}

			bool IsAllowed(const Ruleset& ruleset, const std::vector<std::string>& words)
			{
				if (ruleset.allowed_commands.empty())
					return true;

				// Bingo's own commands
				const auto& name = words[0];
				if (boost::istarts_with(name, "bxt_bingo_") || boost::istarts_with(name, "_bxt_bingo_"))
					return true;

				// Setting a cvar, whose value the cvar rules check
				auto& hw = HwDLL::GetInstance();
				if (hw.FindCVar(name.c_str()))
					return true;

				// The engine only prints "Unknown command" for these
				if (hw.ORIG_Cmd_FindCmd && !hw.ORIG_Cmd_FindCmd(name.c_str()))
					return true;

				return IsInList(ruleset.allowed_commands, words);
			}

			std::string Join(const std::vector<std::string>& commands)
			{
				std::string result;
				for (const auto& command : commands) {
					if (!result.empty())
						result += "; ";
					result += command;
				}

				return result;
			}
		}

		bool Parse(const rapidjson::Value& value, Ruleset& ruleset, std::string& error)
		{
			if (!value.IsObject()) {
				error = "\"ruleset\" must be an object";
				return false;
			}

			if (!ParseBool(value, "single_segment", ruleset.single_segment, error)
				|| !ParseBool(value, "scripted", ruleset.scripted, error)
				|| !ParseBool(value, "no_damage", ruleset.no_damage, error)
				|| !ParseBool(value, "require_kill", ruleset.require_kill, error))
				return false;

			auto cvars = value.FindMember("cvars");
			if (cvars != value.MemberEnd()) {
				if (!cvars->value.IsArray()) {
					error = "\"cvars\" must be a list";
					return false;
				}

				for (const auto& item : cvars->value.GetArray()) {
					auto name = item.IsObject() ? item.FindMember("name") : cvars->value.MemberEnd();
					auto op = item.IsObject() ? item.FindMember("op") : cvars->value.MemberEnd();
					if (!item.IsObject() || name == item.MemberEnd() || !name->value.IsString()
						|| op == item.MemberEnd() || !op->value.IsString()) {
						error = "every cvar rule needs a \"name\" and an \"op\"";
						return false;
					}

					CvarRule rule;
					rule.name = name->value.GetString();
					if (!ParseCvarOp(op->value.GetString(), rule.op)) {
						error = "cvar rule " + rule.name + ": unknown op \"" + op->value.GetString() + "\"";
						return false;
					}

					auto rule_value = item.FindMember("value");
					if (rule_value != item.MemberEnd() && rule_value->value.IsString())
						rule.value = rule_value->value.GetString();
					else if (rule.op != CvarOp::DEFAULT && rule.op != CvarOp::ANY && rule.op != CvarOp::UNCHANGED) {
						error = "cvar rule " + rule.name + ": \"value\" is missing";
						return false;
					}

					ruleset.cvars.push_back(std::move(rule));
				}
			}

			auto commands = value.FindMember("commands");
			if (commands != value.MemberEnd()) {
				if (!commands->value.IsObject()) {
					error = "\"commands\" must be an object";
					return false;
				}

				if (!ParseStringList(commands->value, "allowed", ruleset.allowed_commands, error)
					|| !ParseStringList(commands->value, "not_in_scripts", ruleset.not_in_scripts, error)
					|| !ParseStringList(commands->value, "blocked", ruleset.blocked_commands, error))
					return false;
			}

			return true;
		}

		void Arm(const Ruleset& ruleset)
		{
			cvar_checks.clear();
			if (ruleset.cvars.empty())
				return;

			for (auto cvar = HwDLL::GetInstance().GetCvarList(); cvar; cvar = cvar->next) {
				if (!cvar->name || !cvar->string)
					continue;

				auto rule = FindCvarRule(ruleset, cvar->name);
				if (!rule)
					continue;

				CvarCheck check { cvar, rule->op, rule->value };
				switch (rule->op) {
				case CvarOp::ANY:
					continue;
				case CvarOp::DEFAULT: {
					auto default_value = BxtDefault(cvar->name);
					if (!default_value)
						continue;
					check.value = default_value;
					break;
				}
				case CvarOp::UNCHANGED:
					check.value = cvar->string;
					break;
				default:
					break;
				}

				cvar_checks.push_back(std::move(check));
			}
		}

		std::string CheckCvars()
		{
			for (const auto& check : cvar_checks) {
				std::string current = check.cvar->string ? check.cvar->string : "";
				if (Breaks(check, current))
					return Violation(check, current);
			}

			return {};
		}

		void ApplySetCvars(const Ruleset& ruleset)
		{
			// The server DLL's engine functions set any cvar, like the console does
			auto engfuncs = ServerDLL::GetInstance().pEngfuncs;
			if (!engfuncs || !engfuncs->pfnCVarSetString)
				return;

			auto& hw = HwDLL::GetInstance();
			std::vector<std::string> set;
			for (const auto& rule : ruleset.cvars) {
				// Only exact names of server cvars, so a manifest can't change the player's own settings
				if (rule.op != CvarOp::SET || !boost::istarts_with(rule.name, "sv_") || rule.name.find('*') != std::string::npos)
					continue;

				auto cvar = hw.FindCVar(rule.name.c_str());
				if (!cvar || !cvar->name || !cvar->string)
					continue;

				auto name = boost::algorithm::to_lower_copy(std::string(cvar->name));
				set.push_back(name);
				if (Compare(cvar->string, rule.value) == 0)
					continue;

				// Loading a map sets some of them again (sv_gravity, sv_stepsize, sv_zmax), so this runs every frame
				// The first value seen is the player's own
				set_cvar_originals.emplace(name, cvar->string);
				engfuncs->pfnCVarSetString(cvar->name, rule.value.c_str());
			}

			for (auto it = set_cvar_originals.begin(); it != set_cvar_originals.end();) {
				if (std::find(set.begin(), set.end(), it->first) != set.end()) {
					++it;
					continue;
				}

				engfuncs->pfnCVarSetString(it->first.c_str(), it->second.c_str());
				it = set_cvar_originals.erase(it);
			}
		}

		std::string CheckPlayerCommand(const Ruleset& ruleset, const std::string& text)
		{
			std::vector<std::string> commands;
			Expand(text, commands, 0);
			if (commands.empty())
				return {};

			const char* run_type = ruleset.scripted ? "scripted" : "scriptless";
			for (const auto& command : commands) {
				auto words = Words(command);
				if (!words.empty() && !IsAllowed(ruleset, words))
					return words[0] + " isn't allowed in " + run_type + " runs";
			}

			if (commands.size() > 1) {
				if (!ruleset.scripted)
					return "one key or console line ran " + std::to_string(commands.size()) + " commands (" + Join(commands)
						+ "), scriptless runs only allow one";

				for (const auto& command : commands) {
					auto words = Words(command);
					if (!words.empty() && IsInList(ruleset.not_in_scripts, words))
						return words[0] + " can't be part of a script (" + Join(commands) + ")";
				}
			}

			return {};
		}

		std::string FindBlockedCommand(const Ruleset& ruleset, const std::string& text)
		{
			if (ruleset.blocked_commands.empty())
				return {};

			std::vector<std::string> commands;
			Expand(text, commands, 0);
			for (const auto& command : commands) {
				auto words = Words(command);
				if (!words.empty() && IsInList(ruleset.blocked_commands, words))
					return words[0];
			}

			return {};
		}
	}
}
