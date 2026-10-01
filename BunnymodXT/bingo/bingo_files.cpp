#include "../stdafx.hpp"

#include "bingo_internal.hpp"

namespace Bingo
{
	namespace
	{
		// Missing files are tried again this often
		constexpr long long FILES_RETRY_MS = 15000;

		// Where a file is downloaded from: <files_url><sha256>
		std::string FileUrl(const std::string& sha256)
		{
			auto base = HttpUrl(manifest.files_url.empty() ? std::string("/files/") : manifest.files_url);
			return base.empty() ? std::string() : base + sha256;
		}

		void SendDownloadProgress()
		{
			NetSend(JsonMessage("download_progress", [&](JsonWriter& w) {
				w.Key("done");
				w.Int(FilesReady());
				w.Key("total");
				w.Int(static_cast<int>(net.file_jobs.size()));
			}));
		}

		// Checks, and downloads if needed, the files that aren't there yet
		void SyncMissingFiles()
		{
			std::vector<Platform::FileJob> jobs;
			net.file_sync_jobs.clear();
			for (size_t i = 0; i < net.file_jobs.size(); ++i) {
				if (!net.file_ok[i]) {
					jobs.push_back(net.file_jobs[i]);
					net.file_sync_jobs.push_back(i);
				}
			}

			net.file_events = 0;
			net.files_retry = false;
			net.files = Platform::SyncFiles(std::move(jobs));
		}

		// Prints a file's problem, once until it changes
		// The screen gets a summary at the end of the sync, as a whole board may fail at once
		void FileProblem(size_t job, const std::string& text)
		{
			if (net.file_problems[job] != text) {
				net.file_problems[job] = text;
				net.new_file_problems.push_back(text);
				Print("%s\n", text.c_str());
			}
		}
	}

	// An address from the server as a full one: a path like /files/ is on the server BXT connected to,
	// ws://host/bxt -> http://host/files/
	std::string HttpUrl(const std::string& url)
	{
		if (url.empty() || url[0] != '/')
			return url;

		auto scheme_end = net.url.find("://");
		if (scheme_end == std::string::npos)
			return {};

		auto host_end = net.url.find('/', scheme_end + 3);
		auto host = net.url.substr(scheme_end, host_end == std::string::npos ? std::string::npos : host_end - scheme_end);
		return (net.url.compare(0, scheme_end, "wss") == 0 ? "https" : "http") + host + url;
	}

	int FilesReady()
	{
		return static_cast<int>(std::count(net.file_ok.begin(), net.file_ok.end(), true));
	}

	// Starts getting the manifest's files: the tiles' saves and the extra files
	void StartFiles()
	{
		auto game_dir = GameDir();
		if (game_dir.empty())
			return;

		std::vector<Platform::FileJob> jobs;
		std::vector<std::string> names;
		for (int i = 0; i < TILE_COUNT; ++i) {
			const auto& tile = manifest.tiles[i];
			Platform::FileJob job;
			job.path = SavePath(TileSaveName(tile).c_str());
			// Tiles may share a save
			if (std::any_of(jobs.begin(), jobs.end(), [&](const Platform::FileJob& other) { return other.path == job.path; }))
				continue;

			job.url = FileUrl(tile.save_sha256);
			job.sha256 = tile.save_sha256;
			job.size = tile.save_size;
			// bingo_<hash>.sav is bingo's own
			job.replace = true;
			jobs.push_back(std::move(job));
			names.push_back("the save of " + TileId(i));
		}

		for (const auto& file : manifest.extra_files) {
			Platform::FileJob job;
			job.path = game_dir + "/" + file.path;
			job.url = FileUrl(file.sha256);
			job.sha256 = file.sha256;
			job.size = file.size;
			// It could be a game file or the player's own
			job.replace = false;
			jobs.push_back(std::move(job));
			names.push_back(file.path);
		}

		auto same = [](const Platform::FileJob& a, const Platform::FileJob& b) {
			return a.path == b.path && a.sha256 == b.sha256 && a.size == b.size && a.url == b.url;
		};
		bool unchanged = jobs.size() == net.file_jobs.size() && std::equal(jobs.begin(), jobs.end(), net.file_jobs.begin(), same);

		// A new manifest with the same files (e.g. new handicaps) doesn't stop the downloads
		if (unchanged && (net.files || net.files_retry)) {
			SendDownloadProgress();
			return;
		}

		// Otherwise everything is checked again, in case a file changed since
		net.file_jobs = std::move(jobs);
		net.file_names = std::move(names);
		net.file_ok.assign(net.file_jobs.size(), false);
		if (!unchanged)
			net.file_problems.assign(net.file_jobs.size(), {});
		net.files_downloaded = 0;
		SyncMissingFiles();
		SendDownloadProgress();
	}

	// Called every frame while online: follows the downloads, and tries again when some failed
	void FilesFrame()
	{
		if (!net.files) {
			if (net.files_retry && std::chrono::steady_clock::now() >= net.files_retry_at)
				SyncMissingFiles();
			return;
		}

		using Type = Platform::FileEvent::Type;
		bool progress = false;
		Platform::FileEvent event;
		while (net.files->Poll(event)) {
			if (event.index >= net.file_sync_jobs.size())
				continue;

			++net.file_events;
			auto job = net.file_sync_jobs[event.index];
			const auto& path = net.file_jobs[job].path;
			switch (event.type) {
			case Type::PRESENT:
			case Type::DOWNLOADED:
				net.file_ok[job] = true;
				net.file_problems[job].clear();
				net.files_downloaded += event.type == Type::DOWNLOADED;
				progress = true;
				break;
			case Type::CONFLICT:
				FileProblem(job, path + " is already there, and it isn't the file this game uses. Bingo doesn't replace it: move it somewhere else, and bingo will download its own.");
				break;
			case Type::FAILED:
				FileProblem(job, "Could not download " + net.file_names[job] + ": " + event.text + ".");
				break;
			}
		}

		if (progress)
			SendDownloadProgress();

		if (net.file_events < net.file_sync_jobs.size())
			return;

		net.files.reset();
		if (FilesReady() == static_cast<int>(net.file_jobs.size())) {
			NetSend(JsonMessage("ready", [&](JsonWriter& w) {
				w.Key("manifest_hash");
				w.String(manifest.hash.c_str());
			}));

			if (net.files_downloaded > 0)
				ShowMessage("Downloaded " + std::to_string(net.files_downloaded) + (net.files_downloaded == 1 ? " file" : " files") + ", you're ready.");
			else
				Print("All files are here, you're ready.\n");
		} else {
			net.files_retry = true;
			net.files_retry_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(FILES_RETRY_MS);

			if (!net.new_file_problems.empty()) {
				auto more = net.new_file_problems.size() - 1;
				AddMessage(net.new_file_problems.front());
				AddMessage(more == 0
					? std::string("Bingo tries again every 15 s.")
					: "And " + std::to_string(more) + " more, see the console. Bingo tries again every 15 s.");
			}
		}
		net.new_file_problems.clear();
	}
}
