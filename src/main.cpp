#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#endif

#include <libtorrent/session.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/error_code.hpp>

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/component/event.hpp"

namespace lt = libtorrent;
using namespace ftxui;

// ── Globals ───────────────────────────────────────────────────────────────────

static std::atomic<bool> g_quit{false};

// State visible to the render thread — only written via screen.Post() lambdas
struct TorrentState {
    float       progress     = 0.0f;
    double      dl_rate      = 0.0;
    double      ul_rate      = 0.0;
    int         num_peers    = 0;
    int64_t     total_done   = 0;
    int64_t     total_wanted = 0;
    std::string name;
    std::string state_label  = "...";
    bool        complete     = false;
};

static TorrentState g_state;

// ── Signal handler ────────────────────────────────────────────────────────────

static void on_signal(int) { g_quit = true; }

// ── CLI helpers ───────────────────────────────────────────────────────────────

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s [options] <torrent-file-or-magnet-link>\n"
        "\n"
        "Options:\n"
        "  -o <dir>   Output directory (default: current directory)\n"
        "  -p <port>  Listen port (default: 6881)\n"
        "  -h         Show this help\n",
        prog);
}

static bool is_magnet(const char *uri)
{
    return strncmp(uri, "magnet:", 7) == 0;
}

// ── Format helpers ────────────────────────────────────────────────────────────

static void fmt_rate(double bps, char *buf, size_t len)
{
    if (bps >= 1024 * 1024)
        snprintf(buf, len, "%.1f MB/s", bps / (1024 * 1024));
    else if (bps >= 1024)
        snprintf(buf, len, "%.1f KB/s", bps / 1024);
    else
        snprintf(buf, len, "%.0f B/s", bps);
}

static void fmt_bytes(int64_t bytes, char *buf, size_t len)
{
    if (bytes >= 1024LL * 1024 * 1024)
        snprintf(buf, len, "%.2f GB", (double)bytes / (1024 * 1024 * 1024));
    else if (bytes >= 1024 * 1024)
        snprintf(buf, len, "%.1f MB", (double)bytes / (1024 * 1024));
    else if (bytes >= 1024)
        snprintf(buf, len, "%.1f KB", (double)bytes / 1024);
    else
        snprintf(buf, len, "%" PRId64 " B", bytes);
}

static std::string fmt_eta(int64_t remaining, double rate)
{
    if (rate < 1.0 || remaining <= 0) return "--:--";
    long long s = static_cast<long long>(remaining / rate);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lldm %02llds", s / 60, s % 60);
    return buf;
}

// ── BBS/ANSI TUI element builder ─────────────────────────────────────────────
// Called from the FTXUI render thread; reads g_state which is only mutated
// via screen.Post() lambdas (also on the render thread — no mutex needed).

static Element build_ui(const TorrentState &s)
{
    const int bar_width = 24;
    int filled = static_cast<int>(s.progress * bar_width);
    std::string bar_str;
    for (int i = 0; i < bar_width; ++i)
        bar_str += (i < filled) ? "\xe2\x96\x88" : "\xe2\x96\x91"; // █ / ░

    char pct_buf[16];
    snprintf(pct_buf, sizeof(pct_buf), " %5.1f%%", s.progress * 100.0f);

    char dl_buf[32], ul_buf[32], done_buf[32], total_buf[32];
    fmt_rate(s.dl_rate,       dl_buf,    sizeof(dl_buf));
    fmt_rate(s.ul_rate,       ul_buf,    sizeof(ul_buf));
    fmt_bytes(s.total_done,   done_buf,  sizeof(done_buf));
    fmt_bytes(s.total_wanted, total_buf, sizeof(total_buf));
    std::string eta = fmt_eta(s.total_wanted - s.total_done, s.dl_rate);

    Color state_col = Color::White;
    if      (s.state_label == "DOWNLOADING") state_col = Color::GreenLight;
    else if (s.state_label == "SEEDING")     state_col = Color::MagentaLight;
    else if (s.state_label == "DONE")        state_col = Color::Cyan;
    else if (s.state_label == "ERROR")       state_col = Color::Red;
    else if (s.state_label == "CHECKING" ||
             s.state_label == "METADATA")    state_col = Color::Yellow;

    std::string display_name = s.name.empty() ? "(fetching metadata...)" : s.name;

    auto header = hbox({
        text(" \xe2\x98\x85 TORRENT-DL v1.0 \xe2\x98\x85 ") | bold | color(Color::Cyan),
    }) | hcenter;

    auto name_row = hbox({
        text(" "),
        text(display_name) | color(Color::Yellow) | flex,
    });

    auto bar_row = hbox({
        text(" ["),
        text(bar_str) | color(Color::Green),
        text("]"),
        text(pct_buf) | bold | color(Color::White),
    });

    auto stats1 = hbox({
        text(" DL: ") | color(Color::GrayDark),
        text(dl_buf) | color(Color::GreenLight),
        text("   UL: ") | color(Color::GrayDark),
        text(ul_buf) | color(Color::Yellow),
    });

    auto stats2 = hbox({
        text(" PEERS: ") | color(Color::GrayDark),
        text(std::to_string(s.num_peers)) | color(Color::White),
        text("   ETA: ") | color(Color::GrayDark),
        text(eta) | color(Color::White),
        text("   "),
        text(s.state_label) | bold | color(state_col),
    });

    auto stats3 = hbox({
        text(" ") | color(Color::GrayDark),
        text(done_buf) | color(Color::White),
        text(" / ") | color(Color::GrayDark),
        text(total_buf) | color(Color::White),
    });

    auto footer = hbox({
        text(" [Q] Quit") | color(Color::GrayDark),
    });

    return vbox({
        header,
        separator(),
        name_row,
        separator(),
        bar_row,
        stats1,
        stats2,
        stats3,
        separator(),
        footer,
    }) | borderDouble | color(Color::Cyan);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
    const char *save_path = ".";
    int listen_port = 6881;
    const char *source = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            save_path = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            listen_port = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            source = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!source) {
        fprintf(stderr, "Error: no torrent file or magnet link specified.\n\n");
        print_usage(argv[0]);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    lt::settings_pack sp;
    sp.set_int(lt::settings_pack::alert_mask,
        lt::alert_category::error |
        lt::alert_category::status |
        lt::alert_category::storage);
    sp.set_str(lt::settings_pack::listen_interfaces,
        "0.0.0.0:" + std::to_string(listen_port));

    lt::session ses(sp);

    lt::add_torrent_params atp;
    atp.save_path = save_path;

    lt::error_code ec;

    if (is_magnet(source)) {
        atp = lt::parse_magnet_uri(source, ec);
        if (ec) {
            fprintf(stderr, "Invalid magnet link: %s\n", ec.message().c_str());
            return 1;
        }
        atp.save_path = save_path;
    } else {
        auto ti = std::make_shared<lt::torrent_info>(source, ec);
        if (ec) {
            fprintf(stderr, "Failed to load torrent file: %s\n", ec.message().c_str());
            return 1;
        }
        atp.ti = ti;
        g_state.name = ti->name();
    }

    lt::torrent_handle h = ses.add_torrent(std::move(atp), ec);
    if (ec) {
        fprintf(stderr, "Failed to add torrent: %s\n", ec.message().c_str());
        return 1;
    }

    // ── Windows: ensure UTF-8 for box-drawing characters ─────────────────────
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    // ── FTXUI screen ─────────────────────────────────────────────────────────
    auto screen = ScreenInteractive::Fullscreen();

    auto renderer = Renderer([&] { return build_ui(g_state); });

    auto ui = CatchEvent(renderer, [&](Event e) {
        if (e == Event::Character('q') || e == Event::Character('Q')) {
            g_quit = true;
            screen.ExitLoopClosure()();
            return true;
        }
        return false;
    });

    // ── libtorrent worker — all state writes go via screen.Post() so they
    //    execute on the FTXUI render thread (no mutex needed).
    std::thread lt_thread([&] {
        while (!g_quit) {
            std::vector<lt::alert *> alerts;
            ses.pop_alerts(&alerts);

            for (lt::alert *a : alerts) {
                if (lt::alert_cast<lt::torrent_error_alert>(a)) {
                    screen.Post([&] { g_state.state_label = "ERROR"; });
                } else if (lt::alert_cast<lt::torrent_finished_alert>(a)) {
                    screen.Post([&] {
                        g_state.state_label = "DONE";
                        g_state.complete    = true;
                        g_state.progress    = 1.0f;
                    });
                    screen.Post(Event::Custom);
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    g_quit = true;
                    screen.ExitLoopClosure()();
                    return;
                }
            }

            if (g_quit) break;

            lt::torrent_status st = h.status();

            screen.Post([&, st] {
                g_state.progress     = st.progress;
                g_state.dl_rate      = st.download_rate;
                g_state.ul_rate      = st.upload_rate;
                g_state.num_peers    = st.num_peers;
                g_state.total_done   = st.total_wanted_done;
                g_state.total_wanted = st.total_wanted;
                if (st.has_metadata && g_state.name.empty())
                    g_state.name = st.name;
                switch (st.state) {
                    case lt::torrent_status::checking_files:
                        g_state.state_label = "CHECKING"; break;
                    case lt::torrent_status::downloading_metadata:
                        g_state.state_label = "METADATA"; break;
                    case lt::torrent_status::downloading:
                        g_state.state_label = "DOWNLOADING"; break;
                    case lt::torrent_status::seeding:
                        g_state.state_label = "SEEDING"; break;
                    default: break;
                }
            });
            screen.Post(Event::Custom);

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        screen.ExitLoopClosure()();
    });

    screen.Loop(ui);

    g_quit = true;
    lt_thread.join();

    h.save_resume_data();
    ses.pause();

    return 0;
}
