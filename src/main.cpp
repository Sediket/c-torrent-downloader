#include <cstdio>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

#include <libtorrent/session.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/error_code.hpp>

#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/component_options.hpp"

#include "state.h"
#include "formatters.h"
#include "search.h"
#include "torrent.h"
#include "ui.h"

// ── Global state definitions ──────────────────────────────────────────────────

std::atomic<bool>               g_quit{false};
std::vector<DownloadEntry>      g_downloads;
int                             g_dl_selected  = 0;
std::set<std::string>           g_queued_hashes;
lt::session                    *g_ses          = nullptr;
std::vector<lt::torrent_handle> g_handles;
std::mutex                      g_handles_mutex;
SearchState                     g_search;
int                             g_tab_index    = 0;
std::string                     g_selected_magnet;
std::string                     g_save_path    = ".";
int                             g_listen_port  = 6881;

// ── Signal handler ────────────────────────────────────────────────────────────

static void on_signal(int) { g_quit = true; }

// ── CLI helpers ───────────────────────────────────────────────────────────────

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s [options] [torrent-file-or-magnet-link]\n"
        "\n"
        "Options:\n"
        "  -o <dir>   Output directory (default: current directory)\n"
        "  -p <port>  Listen port (default: 6881)\n"
        "  -h         Show this help\n"
        "\n"
        "Run without arguments to open the interactive search interface.\n",
        prog);
}

static bool is_magnet(const char *uri)
{
    return strncmp(uri, "magnet:", 7) == 0;
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
    const char *source = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            g_save_path = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            g_listen_port = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            source = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    auto screen = ScreenInteractive::Fullscreen();

    // ── If a source was given on CLI, skip straight to download ──────────────
    if (source) {
        lt::session ses(make_session_settings(g_listen_port));
        lt::add_torrent_params atp;
        atp.save_path = g_save_path;
        lt::error_code ec;

        g_downloads.reserve(1);
        DownloadEntry cli_entry;
        cli_entry.index = 0;

        if (is_magnet(source)) {
            atp = lt::parse_magnet_uri(source, ec);
            if (ec) {
                fprintf(stderr, "Invalid magnet link: %s\n", ec.message().c_str());
                return 1;
            }
            atp.save_path = g_save_path;
        } else {
            auto ti = std::make_shared<lt::torrent_info>(source, ec);
            if (ec) {
                fprintf(stderr, "Failed to load torrent file: %s\n", ec.message().c_str());
                return 1;
            }
            atp.ti       = ti;
            cli_entry.name = ti->name();
        }

        lt::torrent_handle h = ses.add_torrent(std::move(atp), ec);
        if (ec) {
            fprintf(stderr, "Failed to add torrent: %s\n", ec.message().c_str());
            return 1;
        }

        cli_entry.handle = h;
        g_downloads.push_back(cli_entry);
        {
            std::lock_guard<std::mutex> lk(g_handles_mutex);
            g_handles.push_back(h);
        }

        auto renderer = Renderer([&] { return build_dl_list_ui(g_downloads, 0); });
        auto ui = CatchEvent(renderer, [&](Event e) {
            if (e == Event::Character('q') || e == Event::Character('Q')) {
                g_quit = true;
                screen.ExitLoopClosure()();
                return true;
            }
            return false;
        });

        std::thread dispatch_thread([&] { run_lt_dispatch(screen, ses); });
        screen.Loop(ui);
        g_quit = true;
        dispatch_thread.join();
        h.save_resume_data();
        ses.pause();
        return 0;
    }

    // ── Search-first mode ─────────────────────────────────────────────────────

    g_downloads.reserve(64);

    g_ses = new lt::session(make_session_settings(g_listen_port));

    std::string search_input_content;

    InputOption input_opt;
    input_opt.multiline = false;

    auto search_input = Input(&search_input_content, "Search torrents...", input_opt);

    // start_download — called when user selects a result from search
    auto start_download = [&](const std::string &magnet,
                               const std::string &info_hash,
                               const std::string &display_name) {
        if (g_queued_hashes.count(info_hash)) return;
        g_queued_hashes.insert(info_hash);

        DownloadEntry entry;
        entry.index       = static_cast<int>(g_downloads.size());
        entry.name        = display_name;
        entry.magnet      = magnet;
        entry.state_label = "queued";
        g_downloads.push_back(entry);

        int idx = entry.index;

        {
            std::lock_guard<std::mutex> lk(g_handles_mutex);
            g_handles.emplace_back();
        }

        std::thread([&screen, idx, magnet] {
            lt::error_code ec;
            lt::add_torrent_params atp = lt::parse_magnet_uri(magnet, ec);
            if (ec) {
                screen.Post([idx] { g_downloads[idx].state_label = "ERROR"; });
                screen.Post(Event::Custom);
                return;
            }
            atp.save_path = g_save_path;

            lt::torrent_handle h = g_ses->add_torrent(std::move(atp), ec);
            if (ec) {
                screen.Post([idx] { g_downloads[idx].state_label = "ERROR"; });
                screen.Post(Event::Custom);
                return;
            }

            {
                std::lock_guard<std::mutex> lk(g_handles_mutex);
                g_handles[idx] = h;
            }
            screen.Post([idx, h] { g_downloads[idx].handle = h; });
            screen.Post(Event::Custom);
        }).detach();
    };

    // Downloads screen component
    auto dl_renderer = Renderer([&] {
        return build_dl_list_ui(g_downloads, g_dl_selected);
    });
    auto dl_component = CatchEvent(dl_renderer, [&](Event e) {
        if (e == Event::Character('q') || e == Event::Character('Q')) {
            g_quit = true;
            screen.ExitLoopClosure()();
            return true;
        }
        if (e == Event::Tab || e == Event::TabReverse ||
            e == Event::ArrowLeft || e == Event::ArrowRight) {
            g_tab_index = (g_tab_index + 1) % 2;
            return true;
        }
        if (e == Event::ArrowDown) {
            if (!g_downloads.empty())
                g_dl_selected = std::min(g_dl_selected + 1, (int)g_downloads.size() - 1);
            return true;
        }
        if (e == Event::ArrowUp) {
            g_dl_selected = std::max(g_dl_selected - 1, 0);
            return true;
        }
        return false;
    });

    // Search screen component
    auto search_renderer = Renderer(search_input, [&] {
        return build_search_ui(g_search, search_input->Render());
    });

    auto search_component = CatchEvent(search_renderer, [&](Event e) {
        if (e == Event::Character('q') || e == Event::Character('Q')) {
            if (search_input_content.empty()) {
                g_quit = true;
                screen.ExitLoopClosure()();
                return true;
            }
            return false;
        }

        if (e == Event::Tab || e == Event::TabReverse) {
            g_tab_index = (g_tab_index + 1) % 2;
            return true;
        }

        if (e == Event::Return) {
            if (g_search.status == SearchStatus::DONE && !g_search.results.empty()) {
                const auto &r = g_search.results[g_search.selected];
                std::string magnet = "magnet:?xt=urn:btih:" + r.info_hash
                    + "&dn=" + url_encode(r.name);
                for (int i = 0; i < TRACKER_COUNT; ++i)
                    magnet += std::string("&tr=") + TRACKERS[i];
                start_download(magnet, r.info_hash, r.name);
                screen.Post(Event::Custom);
                return true;
            }
            trigger_search(screen, search_input_content);
            return true;
        }

        if (g_search.status == SearchStatus::DONE && !g_search.results.empty()) {
            if (e == Event::ArrowDown) {
                g_search.selected = std::min(g_search.selected + 1,
                    (int)g_search.results.size() - 1);
                return true;
            }
            if (e == Event::ArrowUp) {
                g_search.selected = std::max(g_search.selected - 1, 0);
                return true;
            }
        }

        return false;
    });

    // Tab bar + content switcher
    std::vector<std::string> tab_entries = {"[SEARCH]", "[DOWNLOADS]"};
    auto tab_toggle  = Toggle(&tab_entries, &g_tab_index);
    auto tab_content = Container::Tab({search_component, dl_component}, &g_tab_index);
    auto root        = Container::Vertical({tab_toggle, tab_content});
    auto root_renderer = Renderer(root, [&] {
        return vbox({
            tab_toggle->Render() | hcenter,
            separator(),
            tab_content->Render() | flex,
        });
    });

    // Spinner thread — animates while searching
    std::thread spinner_thread([&screen] {
        while (!g_quit) {
            screen.Post([] {
                if (g_search.status == SearchStatus::LOADING)
                    ++g_search.spinner_frame;
            });
            screen.Post(Event::Custom);
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
    });

    // Alert dispatch thread — polls libtorrent and updates g_downloads
    std::thread dispatch_thread([&] { run_lt_dispatch(screen, *g_ses); });

    screen.Loop(root_renderer);

    g_quit = true;
    spinner_thread.join();
    dispatch_thread.join();

    if (g_ses) {
        g_ses->pause();
        delete g_ses;
        g_ses = nullptr;
    }

    return 0;
}
