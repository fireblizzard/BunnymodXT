#include "../../stdafx.hpp"

#include "../bingo.hpp"

// Stub for the native Linux build (HL Steam)
// HL WON on Linux runs the Windows DLL under Wine/Proton,
// so it uses Windows/bingo_platform.cpp instead
namespace Bingo
{
	namespace Platform
	{
		bool IsSupported()
		{
			return false;
		}

		std::string Sha256File(const std::string& path, uint64_t& size)
		{
			size = 0;
			return {};
		}

		bool CaptureInput()
		{
			return false;
		}

		void ReleaseInput()
		{
		}

		namespace
		{
			// Tells the game there's no networking on this build
			class UnsupportedSocket : public Socket
			{
			public:
				UnsupportedSocket()
				{
					failed.type = SocketEvent::Type::FAILED;
					failed.text = "bingo networking isn't supported on this build yet";
				}

				void Send(const std::string& text) override
				{
				}

				void Close(int code) override
				{
				}

				bool Poll(SocketEvent& event) override
				{
					if (reported)
						return false;

					reported = true;
					event = failed;
					return true;
				}

			private:
				SocketEvent failed;
				bool reported = false;
			};
		}

		std::unique_ptr<Socket> OpenSocket(const std::string& url, const std::vector<std::string>& headers)
		{
			return std::make_unique<UnsupportedSocket>();
		}

		namespace
		{
			// Reports every file as failed, there are no downloads on this build
			class UnsupportedFileSync : public FileSync
			{
			public:
				explicit UnsupportedFileSync(size_t count)
					: count(count)
				{
				}

				bool Poll(FileEvent& event) override
				{
					if (next >= count)
						return false;

					event = FileEvent();
					event.type = FileEvent::Type::FAILED;
					event.index = next++;
					event.text = "downloads aren't supported on this build yet";
					return true;
				}

			private:
				size_t count;
				size_t next = 0;
			};
		}

		std::unique_ptr<FileSync> SyncFiles(std::vector<FileJob> jobs)
		{
			return std::make_unique<UnsupportedFileSync>(jobs.size());
		}
	}
}
