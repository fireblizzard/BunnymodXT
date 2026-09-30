#include "../stdafx.hpp"

#include <boost/algorithm/string/predicate.hpp>

#include "../modules/ClientDLL.hpp"
#include "../cvars.hpp"
#include "bingo_internal.hpp"

namespace Bingo
{
	namespace
	{
		// Lowercase and without .sav, since save names aren't case sensitive on Windows
		std::string NormalizeSaveName(std::string name)
		{
			if (boost::iends_with(name, ".sav"))
				name.resize(name.size() - 4);

			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return name;
		}

		// Where the player's own retry save waits while bingo uses that name
		std::string BackupSaveName(const std::string& retry_save)
		{
			return "bingo_backup_" + retry_save;
		}
	}

	// <gamedir>/SAVE/<name>.sav or empty if the game directory isn't known yet
	std::string SavePath(const char* save_name)
	{
		auto game_dir = GameDir();
		if (game_dir.empty())
			return {};

		std::string name = save_name;
		if (name.size() < 4 || name.compare(name.size() - 4, 4, ".sav") != 0)
			name += ".sav";

		return game_dir + "/SAVE/" + name;
	}

	bool ReadFile(const std::string& path, std::string& contents)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
			return false;

		std::ostringstream ss;
		ss << file.rdbuf();
		contents = ss.str();
		return true;
	}

	// The name of a tile's pristine save in SAVE, without .sav
	// The bingo_ prefix keeps it apart from the player's own saves (e.g. a modified practice kit)
	std::string TileSaveName(const Tile& tile)
	{
		return "bingo_" + tile.save_sha256.substr(0, 12);
	}

	// bxt_bingo_retry_save without .sav, lowercase because Wine's case-insensitive files can surprise
	std::string RetrySaveName()
	{
		auto name = NormalizeSaveName(CVars::bxt_bingo_retry_save.GetString());
		return name.empty() ? "hard" : name;
	}

	// The save name a save or load command was given
	std::string SaveNameArg()
	{
		auto engfuncs = ClientDLL::GetInstance().pEngfuncs;
		if (!engfuncs || engfuncs->Cmd_Argc() < 2)
			return {};

		return NormalizeSaveName(engfuncs->Cmd_Argv(1));
	}

	bool FileExists(const std::string& path)
	{
		return std::ifstream(path, std::ios::binary).good();
	}

	bool CopyFile(const std::string& from, const std::string& to)
	{
		std::ifstream in(from, std::ios::binary);
		if (!in)
			return false;

		std::ofstream out(to, std::ios::binary | std::ios::trunc);
		if (!out)
			return false;

		out << in.rdbuf();
		return static_cast<bool>(out.flush());
	}

	FileHash HashSave(const std::string& name)
	{
		FileHash hash;
		if (!name.empty())
			hash.sha256 = Platform::Sha256File(SavePath(name.c_str()), hash.size);
		return hash;
	}

	// Whether the file is exactly the tile's save
	bool IsTileSave(const std::string& path, const Tile& tile)
	{
		uint64_t size;
		auto hash = Platform::Sha256File(path, size);
		return hash == tile.save_sha256 && size == tile.save_size;
	}

	// Keeps the player's own retry save before bingo first overwrites it
	// An existing backup (e.g. after a crash) is the player's original, so it's never replaced
	void BackUpRetrySave(const std::string& retry_save)
	{
		auto backup = SavePath(BackupSaveName(retry_save).c_str());
		auto own = SavePath(retry_save.c_str());
		if (FileExists(backup) || !FileExists(own))
			return;

		if (CopyFile(own, backup))
			EngineDevMsg("[bingo] Backed up %s to %s.\n", own.c_str(), backup.c_str());
		else
			Print("Could not back up %s.\n", own.c_str());
	}

	// Puts the player's own retry save back, if bingo backed it up
	void RestoreRetrySave()
	{
		auto retry_save = RetrySaveName();
		auto backup = SavePath(BackupSaveName(retry_save).c_str());
		auto own = SavePath(retry_save.c_str());
		if (!FileExists(backup))
			return;

		if (CopyFile(backup, own)) {
			std::remove(backup.c_str());
			Print("Restored your own %s.sav.\n", retry_save.c_str());
		} else
			Print("Could not restore %s from %s.\n", own.c_str(), backup.c_str());
	}
}
