#include "../stdafx.hpp"

#include "../modules/ClientDLL.hpp"
#include "../cvars.hpp"
#include "../opengl_utils.hpp"
#include "../triangle_utils.hpp"
#include "bingo_internal.hpp"

namespace Bingo
{
	namespace
	{
		// Default team colors, checked to stay apart from each other and from the unowned grey
		// with simulated protanopia, deuteranopia and tritanopia
		constexpr Rgb DEFAULT_RED = { 230, 75, 40 };
		constexpr Rgb DEFAULT_BLUE = { 40, 110, 230 };

		// Fallbacks for color cvars that can't be parsed, same as the cvar defaults
		constexpr Rgb DEFAULT_UNOWNED = { 40, 40, 40 };
		constexpr Rgb DEFAULT_CURRENT = { 255, 225, 40 };
		constexpr Rgb DEFAULT_SELECTED = { 255, 255, 255 };
		constexpr Rgb DEFAULT_START_TRIGGER = { 255, 105, 180 };
		constexpr Rgb DEFAULT_END_TRIGGER = { 255, 200, 40 };

		SCREENINFO GetScreenInfo()
		{
			SCREENINFO si = {};
			si.iSize = sizeof(si);
			ClientDLL::GetInstance().pEngfuncs->pfnGetScreenInfo(&si);
			return si;
		}

		// Board geometry, in HUD coordinates
		struct Layout
		{
			int x;
			int y;
			int tile_size;
		};

		Layout BoardLayout(const SCREENINFO& si)
		{
			Layout layout;
			layout.tile_size = std::min(si.iWidth, si.iHeight) / 8;
			layout.x = (si.iWidth - layout.tile_size * BOARD_SIZE) / 2;
			layout.y = (si.iHeight - layout.tile_size * BOARD_SIZE) / 2;
			return layout;
		}

		void Fill(const GLUtils& gl, float x, float y, float width, float height)
		{
			gl.rectangle(Vector2D(x, y), Vector2D(x + width, y + height));
		}

		// Contesting squares go in a row from the tile's top-right corner, with the last spot
		// used for a count like "+2" when there are more players than spots
		constexpr int CONTESTING_SPOTS = 4;

		struct ContestingSquares
		{
			int size;
			int margin; // from the tile's edges
			int gap;
		};

		ContestingSquares ContestingLayout(int tile_size)
		{
			int size = std::max(4, tile_size / 10);
			return { size, std::max(3, tile_size / 16), std::max(2, size / 3) };
		}

		// Top-left corner of a spot, counted from the right
		std::pair<int, int> ContestingSpot(int tile_x, int tile_y, int tile_size, int spot)
		{
			auto squares = ContestingLayout(tile_size);
			return {
				tile_x + tile_size - 2 - squares.margin - squares.size - spot * (squares.size + squares.gap),
				tile_y + 2 + squares.margin
			};
		}

		void Outline(const GLUtils& gl, float x, float y, float width, float height, float thickness)
		{
			Fill(gl, x, y, width, thickness);
			Fill(gl, x, y + height - thickness, width, thickness);
			Fill(gl, x, y + thickness, thickness, height - thickness * 2);
			Fill(gl, x + width - thickness, y + thickness, thickness, height - thickness * 2);
		}

		Rgb CvarColor(const CVarWrapper& cvar, Rgb fallback)
		{
			Rgb color;
			return ParseColor(cvar.GetString(), color) ? color : fallback;
		}

		// In order: the player's my_team/other_team override (once on a team),
		// the color the team picked in the frontend, then the default
		// Who's on the tile, grouped by team
		// Offline the local run is added here, online the server's list has everyone
		std::vector<Owner> Contesting(int index)
		{
			auto teams = manifest.tiles[index].contesting;
			if (!Online() && index == current_tile && attempt.state == AttemptState::RUNNING)
				teams.push_back(OfflineTeam());

			std::stable_sort(teams.begin(), teams.end(), [](Owner a, Owner b) { return TeamIndex(a) < TeamIndex(b); });
			return teams;
		}

		Rgb TileColor(Owner owner)
		{
			if (owner == Owner::NONE)
				return CvarColor(CVars::bxt_bingo_color_unowned, DEFAULT_UNOWNED);

			return TeamColor(owner);
		}

		void SetColor(const GLUtils& gl, Rgb color, int alpha)
		{
			gl.color(static_cast<unsigned char>(color.r), static_cast<unsigned char>(color.g), static_cast<unsigned char>(color.b), static_cast<unsigned char>(alpha));
		}

		// Perceived brightness from 0 to 1
		float Luminance(Rgb color)
		{
			// These weights come from https://en.wikipedia.org/wiki/Rec._709
			return (0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b) / 255;
		}

		// Lighter for dark colors, darker for bright ones
		Rgb Highlighted(Rgb color)
		{
			int change = Luminance(color) > 0.6f ? -40 : 40;
			return {
				std::clamp(color.r + change, 0, 255),
				std::clamp(color.g + change, 0, 255),
				std::clamp(color.b + change, 0, 255)
			};
		}

		int TextWidth(const std::string& text)
		{
			int width = 0, height = 0;
			ClientDLL::GetInstance().pEngfuncs->pfnDrawConsoleStringLen(text.c_str(), &width, &height);
			return width;
		}

		void DrawText(int x, int y, const std::string& text, float r, float g, float b)
		{
			auto engfuncs = ClientDLL::GetInstance().pEngfuncs;
			engfuncs->pfnDrawSetTextColor(r, g, b);
			engfuncs->pfnDrawConsoleString(x, y, const_cast<char*>(text.c_str()));
		}

		// The game hides the system cursor, so we'll draw our own for the board...
		// '#' is the outline and 'o' the fill
		const char* const CURSOR[] = {
			"#",
			"##",
			"#o#",
			"#oo#",
			"#ooo#",
			"#oooo#",
			"#ooooo#",
			"#oooooo#",
			"#ooooooo#",
			"#oooooooo#",
			"#ooooooooo#",
			"#oooooo#####",
			"#ooo#oo#",
			"#oo##oo#",
			"#o#  #oo#",
			"##   #oo#",
			"#     #oo#",
			"      #oo#",
			"       ##",
		};

		void DrawCursor(const GLUtils& gl, float x, float y, float scale)
		{
			for (size_t row = 0; row < sizeof(CURSOR) / sizeof(CURSOR[0]); ++row) {
				for (size_t column = 0; CURSOR[row][column]; ++column) {
					char c = CURSOR[row][column];
					if (c == ' ')
						continue;

					if (c == '#')
						gl.color(static_cast<unsigned char>(0), static_cast<unsigned char>(0), static_cast<unsigned char>(0), static_cast<unsigned char>(255));
					else
						gl.color(static_cast<unsigned char>(255), static_cast<unsigned char>(255), static_cast<unsigned char>(255), static_cast<unsigned char>(255));

					Fill(gl, x + column * scale, y + row * scale, scale, scale);
				}
			}
		}

		// Shortens the text to fit, marking the cut with ".."
		std::string FitText(std::string text, int max_width)
		{
			if (TextWidth(text) <= max_width)
				return text;

			while (!text.empty() && TextWidth(text + "..") > max_width)
				text.pop_back();

			return text + "..";
		}
	}

	// The tile under the cursor, or -1
	int HoveredTile()
	{
		if (!board.mouse_seen)
			return -1;

		auto si = GetScreenInfo();
		auto layout = BoardLayout(si);

		// The HUD resolution can differ from the window's
		int x = static_cast<int>(board.mouse_x * si.iWidth) - layout.x;
		int y = static_cast<int>(board.mouse_y * si.iHeight) - layout.y;
		int board_size = layout.tile_size * BOARD_SIZE;
		if (x < 0 || y < 0 || x >= board_size || y >= board_size)
			return -1;

		return y / layout.tile_size * BOARD_SIZE + x / layout.tile_size;
	}

	// "R G B" or "#rrggbb"
	bool ParseColor(const std::string& text, Rgb& color)
	{
		if (!text.empty() && text[0] == '#') {
			if (text.size() != 7 || !std::all_of(text.begin() + 1, text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }))
				return false;

			auto value = std::strtoul(text.c_str() + 1, nullptr, 16);
			color = { static_cast<int>(value >> 16 & 0xff), static_cast<int>(value >> 8 & 0xff), static_cast<int>(value & 0xff) };
			return true;
		}

		int r, g, b;
		char extra;
		if (std::sscanf(text.c_str(), "%d %d %d %c", &r, &g, &b, &extra) != 3)
			return false;

		if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255)
			return false;

		color = { r, g, b };
		return true;
	}

	Rgb TeamColor(Owner team, std::string* source)
	{
		Rgb color;
		if (my_team != Owner::NONE) {
			const auto& cvar = team == my_team ? CVars::bxt_bingo_color_my_team : CVars::bxt_bingo_color_other_team;
			if (ParseColor(cvar.GetString(), color)) {
				if (source)
					*source = team == my_team ? "bxt_bingo_color_my_team" : "bxt_bingo_color_other_team";
				return color;
			}
		}

		const auto& picked = team_colors[TeamIndex(team)];
		if (picked) {
			if (source)
				*source = "picked by the team";
			return *picked;
		}

		if (source)
			*source = "default";
		return team == Owner::RED ? DEFAULT_RED : DEFAULT_BLUE;
	}

	// The match for the mini-board and the board: joining, the countdown, the clock, the end
	std::string MatchStatus()
	{
		auto now = std::chrono::steady_clock::now();
		auto ms_until = [&](std::chrono::steady_clock::time_point when) {
			return std::chrono::duration_cast<std::chrono::milliseconds>(when - now).count();
		};
		auto clock = [](long long ms) {
			char text[32];
			long long seconds = std::max<long long>(0, ms) / 1000;
			std::snprintf(text, sizeof(text), "%lld:%02lld", seconds / 60, seconds % 60);
			return std::string(text);
		};

		switch (net.state) {
		case NetState::OFFLINE:
			return {};
		case NetState::CONNECTING:
			return net.opened ? "Joining..." : "Connecting...";
		case NetState::RETRYING:
			return "Reconnecting in " + std::to_string(std::max<long long>(0, ms_until(net.retry_at) + 999) / 1000) + " s";
		case NetState::JOINED:
			break;
		}

		if (FilesReady() < static_cast<int>(net.file_jobs.size()) && net.lobby_state != "running" && net.lobby_state != "finished")
			return "Downloading " + std::to_string(FilesReady()) + "/" + std::to_string(net.file_jobs.size()) + (net.files_retry ? ", trying again soon" : "");

		if (net.lobby_state == "lobby") {
			int ready = 0, playing = 0;
			for (const auto& player : net.players) {
				if (player.team != Owner::NONE) {
					++playing;
					ready += player.ready && player.connected;
				}
			}
			return "Waiting for the start (" + std::to_string(ready) + "/" + std::to_string(playing) + " ready)";
		}

		if (net.lobby_state == "countdown")
			return "Starts in " + std::to_string(std::max<long long>(0, ms_until(net.starts_at) + 999) / 1000);

		if (net.lobby_state == "finished")
			return net.ending.empty() ? "Game over" : net.ending;

		auto elapsed = net.clock_ms + std::chrono::duration_cast<std::chrono::milliseconds>(now - net.clock_received).count();
		if (net.time_limit_ms < 0)
			return clock(elapsed);
		if (elapsed < net.time_limit_ms)
			return clock(net.time_limit_ms - elapsed + 999) + " left";
		if (net.sudden_death_ms >= 0 && elapsed < net.time_limit_ms + net.sudden_death_ms)
			return "Sudden death " + clock(net.time_limit_ms + net.sudden_death_ms - elapsed + 999);
		return "Time's up";
	}

	void DrawBoard()
	{
		if (!board.open)
			return;

		// Gaps between draws, to see when the board stops being drawn (loading, pause, ...)
		if (frame_count - board.last_drawn_frame > 1) {
			auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - board.last_drawn_time).count();
			Input::DebugPrint("board drawn again after %llu frames (%lld ms), left the map meanwhile: %d\n",
				frame_count - board.last_drawn_frame, static_cast<long long>(ms), board.left_map_since_drawn ? 1 : 0);
		}

		board.left_map_since_drawn = false;

		board.last_drawn_frame = frame_count;
		board.last_drawn_time = std::chrono::steady_clock::now();

		auto si = GetScreenInfo();
		auto layout = BoardLayout(si);
		const int size = layout.tile_size;
		const int char_height = si.iCharHeight;
		const int padding = size / 8;
		const int hovered = HoveredTile();

		{
			GLUtils gl;

			// Backdrop, with room for the title and the hint
			gl.color(static_cast<unsigned char>(0), static_cast<unsigned char>(0), static_cast<unsigned char>(0), static_cast<unsigned char>(170));
			Fill(gl,
				static_cast<float>(layout.x - padding),
				static_cast<float>(layout.y - padding - char_height * 2),
				static_cast<float>(size * BOARD_SIZE + padding * 2),
				static_cast<float>(size * BOARD_SIZE + padding * 2 + char_height * 4));

			for (int i = 0; i < TILE_COUNT; ++i) {
				const auto& tile = manifest.tiles[i];
				float x = static_cast<float>(layout.x + i % BOARD_SIZE * size);
				float y = static_cast<float>(layout.y + i / BOARD_SIZE * size);

				auto color = TileColor(tile.owner);
				SetColor(gl, i == hovered || i == board.selected ? Highlighted(color) : color, 220);
				Fill(gl, x + 2, y + 2, static_cast<float>(size - 4), static_cast<float>(size - 4));

				if (!tile.playable) {
					gl.color(static_cast<unsigned char>(0), static_cast<unsigned char>(0), static_cast<unsigned char>(0), static_cast<unsigned char>(150));
					Fill(gl, x + 2, y + 2, static_cast<float>(size - 4), static_cast<float>(size - 4));
				}

				if (i == current_tile) {
					SetColor(gl, CvarColor(CVars::bxt_bingo_color_current, DEFAULT_CURRENT), 255);
					Outline(gl, x + 2, y + 2, static_cast<float>(size - 4), static_cast<float>(size - 4), 3);
				}

				if (i == board.selected) {
					SetColor(gl, CvarColor(CVars::bxt_bingo_color_selected, DEFAULT_SELECTED), 255);
					Outline(gl, x, y, static_cast<float>(size), static_cast<float>(size), 2);
				}

				// One square per player running the tile, in their team's color
				// with a dark border so it shows on a tile of the same color
				auto contesting = Contesting(i);
				int squares = static_cast<int>(contesting.size()) > CONTESTING_SPOTS ? CONTESTING_SPOTS - 1 : static_cast<int>(contesting.size());
				auto square_size = static_cast<float>(ContestingLayout(size).size);
				for (int spot = 0; spot < squares; ++spot) {
					auto corner = ContestingSpot(static_cast<int>(x), static_cast<int>(y), size, spot);
					gl.color(static_cast<unsigned char>(0), static_cast<unsigned char>(0), static_cast<unsigned char>(0), static_cast<unsigned char>(220));
					Fill(gl, corner.first - 1.0f, corner.second - 1.0f, square_size + 2, square_size + 2);
					SetColor(gl, TeamColor(contesting[spot]), 255);
					Fill(gl, static_cast<float>(corner.first), static_cast<float>(corner.second), square_size, square_size);
				}
			}
		}

		// Text goes after the rectangles, GLUtils restores the texture state the font needs
		auto title = Online() ? "BXT Bingo: " + MatchStatus() : "BXT Bingo: " + manifest.hash;
		DrawText(layout.x, layout.y - padding - char_height * 2 + char_height / 2, title, 1.0f, 0.7f, 0.1f);

		for (int i = 0; i < TILE_COUNT; ++i) {
			const auto& tile = manifest.tiles[i];
			int x = layout.x + i % BOARD_SIZE * size;
			int y = layout.y + i / BOARD_SIZE * size;
			// Dark text on bright tiles, light text otherwise
			// Unplayable tiles are darkened, so they get dimmed light text
			bool dark_text = tile.playable && Luminance(TileColor(tile.owner)) > 0.55f;
			float main = dark_text ? 0.08f : (tile.playable ? 1.0f : 0.5f);
			float secondary = dark_text ? 0.3f : (tile.playable ? 0.65f : 0.35f);
			float holder = dark_text ? 0.15f : (tile.playable ? 0.85f : 0.45f);

			DrawText(x + 6, y + 4, TileId(i), secondary, secondary, secondary);

			auto label = tile.label.empty() ? std::string("?") : FitText(tile.label, size - 8);
			DrawText(x + (size - TextWidth(label)) / 2, y + (size - char_height) / 2, label, main, main, main);

			if (tile.time_ms >= 0) {
				// bxt_bingo_color_time, or the label's automatic color when empty
				Rgb time_color;
				if (ParseColor(CVars::bxt_bingo_color_time.GetString(), time_color)) {
					float dim = tile.playable ? 1.0f : 0.5f;
					DrawText(x + 6, y + size - char_height * 2 - 4, FormatTime(tile.time_ms), time_color.r / 255.0f * dim, time_color.g / 255.0f * dim, time_color.b / 255.0f * dim);
				} else
					DrawText(x + 6, y + size - char_height * 2 - 4, FormatTime(tile.time_ms), main, main, main);
				DrawText(x + 6, y + size - char_height - 4, FitText(tile.holder, size - 12), holder, holder, holder);
			}

			// More players than spots: the last spot says how many more
			auto contesting = Contesting(i);
			if (static_cast<int>(contesting.size()) > CONTESTING_SPOTS) {
				auto more = "+" + std::to_string(contesting.size() - (CONTESTING_SPOTS - 1));
				auto spot = ContestingSpot(x, y, size, CONTESTING_SPOTS - 1);
				auto spot_right = spot.first + ContestingLayout(size).size;
				DrawText(spot_right - TextWidth(more), spot.second, more, main, main, main);
			}
		}

		DrawText(layout.x, layout.y + size * BOARD_SIZE + padding, "Click a tile, or use the arrow keys and Enter. Esc closes the board.", 0.8f, 0.8f, 0.8f);

		// The cursor goes on top
		if (board.mouse_seen) {
			GLUtils gl;
			float scale = static_cast<float>(std::max(1, si.iHeight / 700));
			DrawCursor(gl, board.mouse_x * si.iWidth, board.mouse_y * si.iHeight, scale);
		}
	}

	void DrawTriggers(triangleapi_s* tri)
	{
		if (!CVars::bxt_bingo_show_triggers.GetBool())
			return;

		// The fill defaults much fainter than bxt_triggers (0.3), so the box barely hides what's behind it
		// and the edges show where it is
		auto fill_alpha = std::clamp(CVars::bxt_bingo_triggers_fill_alpha.GetFloat(), 0.0f, 1.0f);
		auto edge_alpha = std::clamp(CVars::bxt_bingo_triggers_edge_alpha.GetFloat(), 0.0f, 1.0f);

		// Additive, so the boxes only brighten what's behind them
		auto draw = [=](const TileTrigger& trigger, Rgb color) {
			if (!trigger.counts_here())
				return;

			auto corners = trigger.get_corner_positions();
			float r = color.r / 255.0f, g = color.g / 255.0f, b = color.b / 255.0f;

			tri->RenderMode(kRenderTransAdd);
			if (fill_alpha > 0) {
				tri->Color4f(r, g, b, fill_alpha);
				TriangleUtils::DrawAACuboid(tri, corners.first, corners.second);
			}

			if (edge_alpha > 0) {
				tri->Color4f(r, g, b, edge_alpha);
				TriangleUtils::DrawAACuboidWireframe(tri, corners.first, corners.second);
			}
		};

		tri->CullFace(TRI_NONE);

		if (start_trigger)
			draw(*start_trigger, CvarColor(CVars::bxt_bingo_color_start_trigger, DEFAULT_START_TRIGGER));
		if (end_trigger)
			draw(*end_trigger, CvarColor(CVars::bxt_bingo_color_end_trigger, DEFAULT_END_TRIGGER));
	}

	void DrawMiniBoard(int x, int y)
	{
		if (board.open)
			return;

		// Online before the board arrives, only how joining goes
		if (!manifest.loaded) {
			if (Online())
				DrawText(x, y, MatchStatus(), 1.0f, 0.7f, 0.1f);
			return;
		}

		auto si = GetScreenInfo();
		const int cell = std::max(8, si.iHeight / 64);
		const int step = cell + 2;
		const int board_size = step * BOARD_SIZE + 2;

		{
			GLUtils gl;

			gl.color(static_cast<unsigned char>(0), static_cast<unsigned char>(0), static_cast<unsigned char>(0), static_cast<unsigned char>(140));
			Fill(gl, static_cast<float>(x), static_cast<float>(y), static_cast<float>(board_size), static_cast<float>(board_size));

			for (int i = 0; i < TILE_COUNT; ++i) {
				float cell_x = static_cast<float>(x + 2 + i % BOARD_SIZE * step);
				float cell_y = static_cast<float>(y + 2 + i / BOARD_SIZE * step);

				// Unplayable tiles are drawn hollow, so they differ by shape and not only color
				const auto& tile = manifest.tiles[i];
				auto color = TileColor(tile.owner);
				if (tile.playable) {
					SetColor(gl, color, 220);
					Fill(gl, cell_x, cell_y, static_cast<float>(cell), static_cast<float>(cell));
				} else {
					// The unowned grey is dark, so its outline is lighter
					SetColor(gl, tile.owner == Owner::NONE ? Highlighted(Highlighted(color)) : color, 255);
					Outline(gl, cell_x + 1, cell_y + 1, static_cast<float>(cell - 2), static_cast<float>(cell - 2), 2);
				}

				if (i == current_tile) {
					SetColor(gl, CvarColor(CVars::bxt_bingo_color_current, DEFAULT_CURRENT), 255);
					Outline(gl, cell_x - 1, cell_y - 1, static_cast<float>(cell + 2), static_cast<float>(cell + 2), 2);
				}
			}
		}

		if (current_tile >= 0) {
			auto color = CvarColor(CVars::bxt_bingo_color_current, DEFAULT_CURRENT);
			DrawText(x, y + board_size + 2, TileName(current_tile), color.r / 255.0f, color.g / 255.0f, color.b / 255.0f);

			std::string state;
			switch (attempt.state) {
			case AttemptState::IDLE:
				break;
			case AttemptState::LOADING:
				state = "Loading";
				break;
			case AttemptState::ARMED:
				state = "Ready";
				break;
			case AttemptState::RUNNING:
				state = attempt.invalid_reason.empty() ? "Running" : "Invalid";
				break;
			case AttemptState::FINISHED:
				state = "Finished";
				break;
			}

			if (!state.empty())
				DrawText(x, y + board_size + 2 + si.iCharHeight, state, 1.0f, 1.0f, 1.0f);
		}

		// The match: countdown, time left, how it ended
		if (Online())
			DrawText(x, y + board_size + 2 + si.iCharHeight * 2, MatchStatus(), 1.0f, 0.7f, 0.1f);

		// Latest messages under the tile lines, fading out at the end
		auto now = std::chrono::steady_clock::now();
		while (!messages.empty() && std::chrono::duration_cast<std::chrono::milliseconds>(now - messages.front().shown).count() > MESSAGE_MS)
			messages.pop_front();

		int line_y = y + board_size + 4 + si.iCharHeight * 3;
		for (const auto& message : messages) {
			auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - message.shown).count();
			float fade = std::min(1.0f, (MESSAGE_MS - age) / 1000.0f);
			DrawText(x, line_y, message.text, 1.0f * fade, 0.85f * fade, 0.3f * fade);
			line_y += si.iCharHeight;
		}
	}
}
