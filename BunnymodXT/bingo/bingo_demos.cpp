#include "../stdafx.hpp"

#include <ctime>

#include "../modules/ClientDLL.hpp"
#include "../modules/HwDLL.hpp"
#include "../git_revision.hpp"
#include "../runtime_data.hpp"
#include "bingo_internal.hpp"

// Every online attempt is recorded, from the load of the tile's save
// to its end, as bingo_<attempt_id>_1.dem and on (a new part after each load, like bxt_autorecord)
// Demos of runs that reach the end trigger stay, the others are deleted
// The server asks for some of them afterwards, and they're sent with an HTTP PUT per part

namespace Bingo
{
	namespace
	{
		// Demos of attempts that ended without a time, deleted once the engine has closed them
		std::vector<std::string> discarded;

		// Runtime data waiting for the demo to start, which is a few frames after the load
		std::vector<std::string> waiting_info;

		// Set while a command that loads or restarts the map runs, which closes the demo by itself
		bool map_ending = false;
		std::chrono::steady_clock::time_point next_delete;

		// A failed upload is tried again after a pause, a few times
		constexpr long long UPLOAD_RETRY_MS = 15000;
		constexpr int UPLOAD_TRIES = 3;

		// The most parts a demo is looked for, one per load in a segmented run
		constexpr size_t MAX_DEMO_PARTS = 1000;

		std::string DemoPath(const std::string& name, size_t part)
		{
			return GameDir() + "/" + name + "_" + std::to_string(part) + ".dem";
		}

		// The parts of an attempt's demo, in order
		std::vector<std::string> DemoParts(const std::string& attempt_id)
		{
			std::vector<std::string> parts;
			for (size_t part = 1; part <= MAX_DEMO_PARTS; ++part) {
				auto path = DemoPath("bingo_" + attempt_id, part);
				if (!FileExists(path))
					break;
				parts.push_back(path);
			}
			return parts;
		}

		// Deletes the parts that aren't open any more, true once they're all gone
		bool DeleteDemo(const std::string& name)
		{
			bool all = true;
			for (size_t part = 1; part <= MAX_DEMO_PARTS; ++part) {
				auto path = DemoPath(name, part);
				if (!FileExists(path))
					break;
				if (std::remove(path.c_str()) != 0)
					all = false;
			}
			return all;
		}

		// UTC, like 2026-10-01T14:32:10Z
		std::string UtcNow()
		{
			auto now = std::time(nullptr);
			char text[32] = {};
			std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
			return text;
		}

		// Runtime data is only kept while a demo is being recorded, and only bingo's own demo gets it
		void FlushInfo()
		{
			auto& hw = HwDLL::GetInstance();
			if (waiting_info.empty() || !hw.IsRecordingDemo() || !hw.IsAutoRecording(attempt.demo))
				return;

			for (auto& json : waiting_info)
				RuntimeData::Add(RuntimeData::BingoInfo{ std::move(json) });
			waiting_info.clear();
		}

		void SendDemoUnavailable(const std::string& attempt_id, const std::string& reason)
		{
			// The server takes 1 to 200 characters, without control characters
			std::string text;
			for (char c : reason) {
				if (static_cast<unsigned char>(c) >= 32 && c != 127 && text.size() < 200)
					text += c;
			}

			NetSend(JsonMessage("demo_unavailable", [&](JsonWriter& w) {
				w.Key("attempt_id");
				w.String(attempt_id.c_str());
				w.Key("reason");
				w.String(text.empty() ? "unknown" : text.c_str());
			}));
		}

		// Sends the next demo the server asked for
		void StartUpload()
		{
			const auto& request = net.demo_requests.front();

			// A demo that's still being recorded waits for the end of its run
			if (request.attempt_id == attempt.id && (attempt.state == AttemptState::RUNNING || HwDLL::GetInstance().IsRecordingDemo()))
				return;

			auto parts = DemoParts(request.attempt_id);
			if (parts.empty()) {
				Print("The server asked for the demo of a run, and it isn't in %s any more.\n", GameDir().c_str());
				SendDemoUnavailable(request.attempt_id, "the demo isn't on the player's PC");
				net.demo_requests.pop_front();
				return;
			}

			std::vector<Platform::UploadJob> jobs;
			for (size_t i = 0; i < parts.size(); ++i) {
				Platform::UploadJob job;
				job.url = HttpUrl(request.url);
				job.path = parts[i];
				job.headers.push_back("X-Bingo-Session: " + net.session_token);
				job.headers.push_back("X-Bingo-Demo-Part: " + std::to_string(i + 1) + "/" + std::to_string(parts.size()));
				jobs.push_back(std::move(job));
			}

			net.upload_parts = jobs.size();
			net.upload_done = 0;
			net.upload = Platform::UploadFiles(std::move(jobs));
		}

		// Follows the upload that's running
		void UploadFrame()
		{
			Platform::UploadEvent event;
			while (net.upload && net.upload->Poll(event)) {
				const auto attempt_id = net.demo_requests.front().attempt_id;
				if (event.ok) {
					if (++net.upload_done < net.upload_parts)
						continue;

					NetSend(JsonMessage("demo_uploaded", [&](JsonWriter& w) {
						w.Key("attempt_id");
						w.String(attempt_id.c_str());
						w.Key("parts");
						w.Int(static_cast<int>(net.upload_parts));
					}));
					Print("Sent the demo of a run to the server, as it asked.\n");
				} else if (++net.upload_tries < UPLOAD_TRIES) {
					Print("Could not send a demo to the server: %s. Trying again in %d s.\n", event.text.c_str(), static_cast<int>(UPLOAD_RETRY_MS / 1000));
					net.upload_retry_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(UPLOAD_RETRY_MS);
					net.upload.reset();
					return;
				} else {
					Print("Could not send a demo to the server: %s.\n", event.text.c_str());
					SendDemoUnavailable(attempt_id, "the upload failed: " + event.text);
				}

				net.upload.reset();
				net.upload_tries = 0;
				net.demo_requests.pop_front();
				return;
			}
		}
	}

	// Records the new attempt from its load on
	// Called before the tile's save loads, when the attempt has its id
	void StartDemo()
	{
		static bool told = false;
		if (!told) {
			told = true;
			Print("Bingo records each online run as bingo_<run id>_1.dem (and _2 and on after loads) in %s. The demos of runs that reach the end stay there.\n", GameDir().c_str());
		}

		waiting_info.clear();
		attempt.demo = "bingo_" + attempt.id;
		HwDLL::GetInstance().AutoRecordAfterLoad(attempt.demo);
	}

	// Writes what the run is into its demo, as BXT runtime data
	void AddDemoInfo(const char* event, const std::function<void(JsonWriter&)>& write)
	{
		if (attempt.demo.empty())
			return;

		rapidjson::StringBuffer buffer;
		JsonWriter writer(buffer);
		writer.StartObject();
		writer.Key("event");
		writer.String(event);
		writer.Key("attempt_id");
		writer.String(attempt.id.c_str());
		writer.Key("utc");
		writer.String(UtcNow().c_str());
		write(writer);
		writer.EndObject();
		waiting_info.push_back(buffer.GetString());
		FlushInfo();
	}

	// Stops the attempt's demo, keeping it for a run with a time
	void EndDemo(bool keep)
	{
		if (attempt.demo.empty())
			return;

		FlushInfo();
		waiting_info.clear();

		// The stop command through the command buffer, the way the engine expects it
		auto& hw = HwDLL::GetInstance();
		hw.StopAutoRecord();
		if (hw.IsRecordingDemo() && !map_ending)
			ClientDLL::GetInstance().pEngfuncs->pfnClientCmd(const_cast<char*>("stop\n"));

		if (!keep) {
			discarded.push_back(attempt.demo);
			attempt.demo.clear();
		}
	}

	// Around the load, map and restart commands
	void SetMapEnding(bool ending)
	{
		map_ending = ending;
	}

	bool AllowDemoCommand(const char* command, const char* demo_name)
	{
		// Bingo's autorecording goes from the tile's load to the end of the run
		if (!HwDLL::GetInstance().IsAutoRecording(attempt.demo))
			return true;

		// Its own next part, after a load
		std::string name = demo_name ? demo_name : "";
		if (!std::strcmp(command, "record") && name.compare(0, attempt.demo.size() + 1, attempt.demo + "_") == 0)
			return true;

		Print("%s is blocked during online runs, bingo is recording this one. You can record again after it ends.\n", command);
		return false;
	}

	void RequestDemo(const std::string& attempt_id, const std::string& url)
	{
		for (const auto& request : net.demo_requests) {
			if (request.attempt_id == attempt_id)
				return;
		}

		net.demo_requests.push_back({ attempt_id, url });
	}

	// Called every frame. Writes the runtime data, deletes the demos that go,
	// and sends the ones the server asked for
	void DemosFrame()
	{
		FlushInfo();

		auto now = std::chrono::steady_clock::now();
		if (!discarded.empty() && now >= next_delete) {
			next_delete = now + std::chrono::seconds(1);
			discarded.erase(std::remove_if(discarded.begin(), discarded.end(), DeleteDemo), discarded.end());
		}

		if (net.upload) {
			UploadFrame();
			return;
		}

		if (!net.demo_requests.empty() && net.state == NetState::JOINED && now >= net.upload_retry_at)
			StartUpload();
	}
}
