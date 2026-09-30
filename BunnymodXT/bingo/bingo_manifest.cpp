#include "../stdafx.hpp"

#include <boost/algorithm/string/split.hpp>
#include <rapidjson/error/en.h>

#include "bingo_internal.hpp"

// Manifest parsing, same JSON shape as the server's manifest message (src/protocol in bxt-bingo-server)
// Each function returns false with the problem in `error`

namespace Bingo
{
	namespace
	{
		bool IsSha256(const std::string& s)
		{
			return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
				return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
			});
		}

		bool ParseTriggerBox(const rapidjson::Value& value, TriggerBox& box, std::string& error)
		{
			if (!value.IsObject()) {
				error = "isn't an object";
				return false;
			}

			// No map means any map, like the practice kit triggers
			if (value.HasMember("map") && !GetString(value, "map", box.map, error))
				return false;

			auto corners = value.FindMember("corners");
			if (corners == value.MemberEnd() || !corners->value.IsArray() || corners->value.Size() != 2) {
				error = "\"corners\" must be a list of two points";
				return false;
			}

			for (rapidjson::SizeType i = 0; i < 2; ++i) {
				const auto& point = corners->value[i];
				if (!point.IsArray() || point.Size() != 3) {
					error = "each corner must be three numbers";
					return false;
				}

				for (rapidjson::SizeType j = 0; j < 3; ++j) {
					if (!point[j].IsNumber()) {
						error = "each corner must be three numbers";
						return false;
					}

					box.corners[i][j] = point[j].GetFloat();
				}
			}

			return true;
		}

		bool ParseTile(const rapidjson::Value& value, int& index, Tile& tile, std::string& error)
		{
			if (!value.IsObject()) {
				error = "isn't an object";
				return false;
			}

			std::string id;
			if (!GetString(value, "id", id, error))
				return false;

			index = ParseTileId(id);
			if (index < 0) {
				error = "\"" + id + "\" isn't a tile id (A1 to E5)";
				return false;
			}

			// null while the labels are hidden until the round starts
			auto label = value.FindMember("label");
			if (label != value.MemberEnd() && label->value.IsString())
				tile.label.assign(label->value.GetString(), label->value.GetStringLength());
			else if (label != value.MemberEnd() && !label->value.IsNull()) {
				error = "tile " + id + ": \"label\" must be a string or null";
				return false;
			}

			auto save = value.FindMember("save");
			if (save == value.MemberEnd() || !save->value.IsObject()) {
				error = "tile " + id + ": \"save\" is missing or isn't an object";
				return false;
			}

			if (!GetString(save->value, "sha256", tile.save_sha256, error)) {
				error = "tile " + id + ": save: " + error;
				return false;
			}

			if (!IsSha256(tile.save_sha256)) {
				error = "tile " + id + ": the save's sha256 must be 64 lowercase hex digits";
				return false;
			}

			auto size = save->value.FindMember("size");
			if (size == save->value.MemberEnd() || !size->value.IsUint64()) {
				error = "tile " + id + ": the save's size is missing or isn't a number";
				return false;
			}
			tile.save_size = size->value.GetUint64();

			auto start = value.FindMember("start");
			if (start == value.MemberEnd() || !start->value.IsObject()) {
				error = "tile " + id + ": \"start\" is missing or isn't an object";
				return false;
			}

			std::string start_type;
			if (!GetString(start->value, "type", start_type, error)) {
				error = "tile " + id + ": start: " + error;
				return false;
			}

			if (start_type == "on_load")
				tile.start_on_load = true;
			else if (start_type == "trigger") {
				if (!ParseTriggerBox(start->value, tile.start, error)) {
					error = "tile " + id + ": start: " + error;
					return false;
				}
			} else {
				error = "tile " + id + ": unknown start type \"" + start_type + "\"";
				return false;
			}

			auto end = value.FindMember("end");
			if (end == value.MemberEnd()) {
				error = "tile " + id + ": \"end\" is missing";
				return false;
			}

			// A trigger box may leave the type out
			std::string end_type = "trigger";
			auto end_type_member = end->value.IsObject() ? end->value.FindMember("type") : end->value.MemberEnd();
			if (end->value.IsObject() && end_type_member != end->value.MemberEnd() && !GetString(end->value, "type", end_type, error)) {
				error = "tile " + id + ": end: " + error;
				return false;
			}

			if (end_type == "game_end")
				tile.end_on_game_end = true;
			else if (end_type != "trigger") {
				error = "tile " + id + ": unknown end type \"" + end_type + "\"";
				return false;
			} else if (!ParseTriggerBox(end->value, tile.end, error)) {
				error = "tile " + id + ": end: " + error;
				return false;
			}

			return true;
		}

		// Like the server's isSafeExtraPath: lowercase, forward slashes, no "..", a known extension
		// So a file from the server can't land outside the game directory, or be something that runs
		bool IsSafeExtraPath(const std::string& path)
		{
			static const char* const EXTENSIONS[] = { "wav" };

			std::vector<std::string> parts;
			boost::algorithm::split(parts, path, [](char c) { return c == '/'; });
			if (parts.size() < 2)
				return false;

			for (size_t i = 0; i < parts.size(); ++i) {
				const auto& part = parts[i];
				if (part.empty() || part == "." || part == "..")
					return false;

				// Dots only after the first folder
				for (char c : part) {
					if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || (c == '.' && i > 0)))
						return false;
				}
			}

			auto dot = parts.back().rfind('.');
			if (dot == std::string::npos)
				return false;

			auto extension = parts.back().substr(dot + 1);
			return std::any_of(std::begin(EXTENSIONS), std::end(EXTENSIONS), [&](const char* e) { return extension == e; });
		}

		bool ParseExtraFiles(const rapidjson::Value& doc, std::vector<ExtraFile>& result, std::string& error)
		{
			auto files = doc.FindMember("extra_files");
			if (files == doc.MemberEnd())
				return true;

			if (!files->value.IsArray()) {
				error = "\"extra_files\" must be a list";
				return false;
			}

			for (const auto& value : files->value.GetArray()) {
				ExtraFile file;
				if (!value.IsObject() || !GetString(value, "path", file.path, error) || !GetString(value, "sha256", file.sha256, error)) {
					error = "extra_files: " + (value.IsObject() ? error : std::string("a file isn't an object"));
					return false;
				}

				if (!IsSafeExtraPath(file.path)) {
					error = "extra_files: " + file.path + " isn't a path bingo may write to";
					return false;
				}

				if (!IsSha256(file.sha256)) {
					error = "extra_files: " + file.path + ": the sha256 must be 64 lowercase hex digits";
					return false;
				}

				auto size = value.FindMember("size");
				if (size == value.MemberEnd() || !size->value.IsUint64()) {
					error = "extra_files: " + file.path + ": the size is missing or isn't a number";
					return false;
				}
				file.size = size->value.GetUint64();

				result.push_back(std::move(file));
			}

			return true;
		}
	}

	bool GetString(const rapidjson::Value& object, const char* name, std::string& out, std::string& error)
	{
		auto it = object.FindMember(name);
		if (it == object.MemberEnd() || !it->value.IsString()) {
			error = std::string("\"") + name + "\" is missing or isn't a string";
			return false;
		}

		out.assign(it->value.GetString(), it->value.GetStringLength());
		return true;
	}

	bool ParseManifest(const std::string& text, Manifest& result, std::string& error)
	{
		rapidjson::Document doc;
		doc.Parse(text.c_str());
		if (doc.HasParseError()) {
			error = std::string("invalid JSON at offset ") + std::to_string(doc.GetErrorOffset()) + ": " + rapidjson::GetParseError_En(doc.GetParseError());
			return false;
		}

		if (!doc.IsObject()) {
			error = "the file must contain a JSON object";
			return false;
		}

		// A server message has "type": "manifest", a local file may leave it out
		auto type = doc.FindMember("type");
		if (type != doc.MemberEnd() && !(type->value.IsString() && std::string(type->value.GetString()) == "manifest")) {
			error = "\"type\" must be \"manifest\"";
			return false;
		}

		if (!GetString(doc, "manifest_hash", result.hash, error))
			return false;

		auto tiles = doc.FindMember("tiles");
		if (tiles == doc.MemberEnd() || !tiles->value.IsArray() || tiles->value.Size() != TILE_COUNT) {
			error = "\"tiles\" must be a list of " + std::to_string(TILE_COUNT) + " tiles";
			return false;
		}

		std::array<bool, TILE_COUNT> seen = {};
		for (const auto& value : tiles->value.GetArray()) {
			int index;
			Tile tile;
			if (!ParseTile(value, index, tile, error))
				return false;

			if (seen[index]) {
				error = "tile " + TileId(index) + " is listed twice";
				return false;
			}

			seen[index] = true;
			result.tiles[index] = std::move(tile);
		}

		auto ruleset = doc.FindMember("ruleset");
		if (ruleset != doc.MemberEnd() && !Rules::Parse(ruleset->value, result.ruleset, error)) {
			error = "ruleset: " + error;
			return false;
		}

		if (!ParseExtraFiles(doc, result.extra_files, error))
			return false;

		if (doc.HasMember("files_url") && !GetString(doc, "files_url", result.files_url, error))
			return false;

		if (doc.HasMember("game") && !GetString(doc, "game", result.game, error))
			return false;

		result.loaded = true;
		return true;
	}
}
