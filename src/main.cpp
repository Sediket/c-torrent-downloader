#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>
#include <set>
#include <mutex>
#include <chrono>
#include <thread>
#include <atomic>
#include <stdexcept>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
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
#include "ftxui/component/component_options.hpp"

#include <nlohmann/json.hpp>

namespace lt = libtorrent;
using namespace ftxui;

// ── Globals ───────────────────────────────────────────────────────────────────

static std::atomic<bool> g_quit{false};

// Per-download entry — written only via screen.Post() lambdas
struct DownloadEntry {
    int                index        = 0;
    std::string        name;
    std::string        magnet;
    lt::torrent_handle handle;
    float              progress     = 0.0f;
    double             dl_rate      = 0.0;
    double             ul_rate      = 0.0;
    int                num_peers    = 0;
    int64_t            total_done   = 0;
    int64_t            total_wanted = 0;
    std::string        state_label  = "queued";
    bool               complete     = false;
};

// All downloads ever started — append-only, reserve(64) prevents reallocation
static std::vector<DownloadEntry>      g_downloads;
static int                             g_dl_selected  = 0;
static std::set<std::string>           g_queued_hashes;

// Shared libtorrent session and handle list (protected by mutex)
static lt::session                    *g_ses          = nullptr;
static std::vector<lt::torrent_handle> g_handles;
static std::mutex                      g_handles_mutex;

// Search state
struct SearchResult {
    std::string id, name, info_hash, category;
    int     seeders  = 0;
    int     leechers = 0;
    int64_t size     = 0;
};

enum class SearchStatus { IDLE, LOADING, DONE, ERR };

struct SearchState {
    std::string               query;
    std::vector<SearchResult> results;
    int                       selected      = 0;
    SearchStatus              status        = SearchStatus::IDLE;
    std::string               error_msg;
    size_t                    spinner_frame = 0;
};

static SearchState  g_search;
static int          g_tab_index    = 0;   // 0 = search, 1 = downloads
static std::string  g_selected_magnet;
static std::string  g_save_path    = ".";
static int          g_listen_port  = 6881;

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
        snprintf(buf, len, "%.2f GB", (double)bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024 * 1024)
        snprintf(buf, len, "%.1f MB", (double)bytes / (1024.0 * 1024));
    else if (bytes >= 1024)
        snprintf(buf, len, "%.1f KB", (double)bytes / 1024.0);
    else
        snprintf(buf, len, "%" PRId64 " B", bytes);
}

// ── URL encode ────────────────────────────────────────────────────────────────

static std::string url_encode(const std::string &s)
{
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// ── WinHTTP GET ───────────────────────────────────────────────────────────────

#ifdef _WIN32
static std::string winhttp_get(const std::wstring &host, const std::wstring &path)
{
    struct HGuard {
        HINTERNET h;
        ~HGuard() { if (h) WinHttpCloseHandle(h); }
    };

    HINTERNET hSess = WinHttpOpen(L"torrent-dl/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) throw std::runtime_error("WinHttpOpen failed");
    HGuard gs{hSess};

    HINTERNET hConn = WinHttpConnect(hSess, host.c_str(),
        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConn) throw std::runtime_error("WinHttpConnect failed");
    HGuard gc{hConn};

    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hReq) throw std::runtime_error("WinHttpOpenRequest failed");
    HGuard gr{hReq};

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        throw std::runtime_error("WinHttpSendRequest failed");

    if (!WinHttpReceiveResponse(hReq, nullptr))
        throw std::runtime_error("WinHttpReceiveResponse failed");

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
        WINHTTP_NO_HEADER_INDEX);
    if (status != 200)
        throw std::runtime_error("HTTP " + std::to_string(status));

    std::string body;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
        std::string chunk(avail, '\0');
        DWORD read = 0;
        WinHttpReadData(hReq, &chunk[0], avail, &read);
        chunk.resize(read);
        body += chunk;
    }
    return body;
}
#endif

// ── Search trigger ────────────────────────────────────────────────────────────

static void trigger_search(ScreenInteractive &screen, const std::string &query)
{
    if (query.empty()) return;
    if (g_search.status == SearchStatus::LOADING) return;

    g_search.status       = SearchStatus::LOADING;
    g_search.results.clear();
    g_search.selected     = 0;
    g_search.spinner_frame = 0;

    std::string q = query;
    std::thread([&screen, q] {
        try {
#ifdef _WIN32
            std::string path_str = "/q.php?q=" + url_encode(q) + "&cat=0";
            std::wstring wpath(path_str.begin(), path_str.end());
            std::string body = winhttp_get(L"apibay.org", wpath);
#else
            (void)q;
            std::string body = "[]";
#endif
            auto j = nlohmann::json::parse(body);
            std::vector<SearchResult> results;

            for (auto &item : j) {
                std::string id = item.value("id", "0");
                if (id == "0") break;

                SearchResult r;
                r.id        = id;
                r.name      = item.value("name",      "");
                r.info_hash = item.value("info_hash",  "");
                r.category  = item.value("category",   "");
                try { r.seeders  = std::stoi(item.value("seeders",  "0")); } catch (...) {}
                try { r.leechers = std::stoi(item.value("leechers", "0")); } catch (...) {}
                try { r.size     = std::stoll(item.value("size",    "0")); } catch (...) {}
                results.push_back(std::move(r));
            }

            screen.Post([results = std::move(results)] {
                g_search.results  = std::move(results);
                g_search.status   = SearchStatus::DONE;
                g_search.selected = 0;
            });
        } catch (const std::exception &ex) {
            std::string msg = ex.what();
            screen.Post([msg] {
                g_search.status    = SearchStatus::ERR;
                g_search.error_msg = msg;
            });
        }
        screen.Post(Event::Custom);
    }).detach();
}

// ── libtorrent alert dispatch loop (single thread handles all torrents) ───────

static void run_lt_dispatch(ScreenInteractive &screen, lt::session &ses)
{
    while (!g_quit) {
        std::vector<lt::alert *> alerts;
        ses.pop_alerts(&alerts);

        for (lt::alert *a : alerts) {
            auto *ta = lt::alert_cast<lt::torrent_alert>(a);
            if (!ta) continue;
            lt::torrent_handle ah = ta->handle;

            int idx = -1;
            {
                std::lock_guard<std::mutex> lk(g_handles_mutex);
                for (int i = 0; i < (int)g_handles.size(); ++i) {
                    if (g_handles[i] == ah) { idx = i; break; }
                }
            }
            if (idx < 0) continue;

            if (lt::alert_cast<lt::torrent_error_alert>(a)) {
                screen.Post([idx] { g_downloads[idx].state_label = "ERROR"; });
            } else if (lt::alert_cast<lt::torrent_finished_alert>(a)) {
                screen.Post([idx] {
                    g_downloads[idx].state_label = "DONE";
                    g_downloads[idx].complete    = true;
                    g_downloads[idx].progress    = 1.0f;
                });
            }
        }

        // Poll status for all active torrents
        {
            std::lock_guard<std::mutex> lk(g_handles_mutex);
            for (int idx = 0; idx < (int)g_handles.size(); ++idx) {
                if (g_downloads[idx].complete) continue;
                lt::torrent_status st = g_handles[idx].status();
                screen.Post([st, idx] {
                    g_downloads[idx].progress     = st.progress;
                    g_downloads[idx].dl_rate      = st.download_rate;
                    g_downloads[idx].ul_rate      = st.upload_rate;
                    g_downloads[idx].num_peers    = st.num_peers;
                    g_downloads[idx].total_done   = st.total_wanted_done;
                    g_downloads[idx].total_wanted = st.total_wanted;
                    if (st.has_metadata && g_downloads[idx].name.empty())
                        g_downloads[idx].name = st.name;
                    switch (st.state) {
                        case lt::torrent_status::checking_files:
                            g_downloads[idx].state_label = "CHECKING";    break;
                        case lt::torrent_status::downloading_metadata:
                            g_downloads[idx].state_label = "METADATA";    break;
                        case lt::torrent_status::downloading:
                            g_downloads[idx].state_label = "DOWNLOADING"; break;
                        case lt::torrent_status::seeding:
                            g_downloads[idx].state_label = "SEEDING";     break;
                        default: break;
                    }
                });
            }
        }

        screen.Post(Event::Custom);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    screen.ExitLoopClosure()();
}

// ── Downloads list UI builder ─────────────────────────────────────────────────

static Element render_dl_row(const DownloadEntry &e, bool selected)
{
    const int bar_width = 12;
    int filled = static_cast<int>(e.progress * bar_width);
    std::string bar_str;
    for (int i = 0; i < bar_width; ++i)
        bar_str += (i < filled) ? "\xe2\x96\x88" : "\xe2\x96\x91";

    char pct_buf[8];
    snprintf(pct_buf, sizeof(pct_buf), "%5.1f%%", e.progress * 100.0f);

    char dl_buf[16];
    fmt_rate(e.dl_rate, dl_buf, sizeof(dl_buf));

    Color state_col = Color::White;
    if      (e.state_label == "DOWNLOADING") state_col = Color::GreenLight;
    else if (e.state_label == "SEEDING")     state_col = Color::MagentaLight;
    else if (e.state_label == "DONE")        state_col = Color::Cyan;
    else if (e.state_label == "ERROR")       state_col = Color::Red;
    else if (e.state_label == "CHECKING" ||
             e.state_label == "METADATA")    state_col = Color::Yellow;
    else if (e.state_label == "queued")      state_col = Color::GrayDark;

    std::string display_name = e.name.empty() ? "(fetching metadata...)" : e.name;

    auto row = hbox({
        text(selected ? " \xe2\x96\xba " : "   ") | color(Color::Cyan),
        text(display_name) | color(Color::White) | flex,
        text(" ["),
        text(bar_str) | color(Color::Green),
        text("] "),
        text(pct_buf) | color(Color::White) | size(WIDTH, EQUAL, 7),
        text("  "),
        text(dl_buf) | color(Color::GreenLight) | size(WIDTH, EQUAL, 10),
        text("  "),
        text(e.state_label) | bold | color(state_col) | size(WIDTH, EQUAL, 12),
    });

    if (selected) return row | inverted;
    return row;
}

static Element build_dl_list_ui(const std::vector<DownloadEntry> &downloads, int selected)
{
    auto header = hbox({
        text(" \xe2\x98\x85 TORRENT-DL v1.0 \xe2\x98\x85 ") | bold | color(Color::Cyan),
    }) | hcenter;

    Element body;

    if (downloads.empty()) {
        body = vbox({
            filler(),
            hbox({
                filler(),
                text("  \xe2\x97\x86 No downloads yet.") | color(Color::GrayDark),
                text(" Switch to Search") | color(Color::Cyan),
                text(" and press") | color(Color::GrayDark),
                text(" [ENTER]") | color(Color::Cyan) | bold,
                text(" on a result.  \xe2\x97\x86") | color(Color::GrayDark),
                filler(),
            }),
            filler(),
        }) | flex;
    } else {
        auto col_header = hbox({
            text("   ") | color(Color::Cyan),
            text("NAME") | color(Color::Cyan) | bold | flex,
            text(" PROGRESS       ") | color(Color::Cyan) | bold,
            text("  DL          ") | color(Color::Cyan) | bold,
            text("  STATE       ") | color(Color::Cyan) | bold,
        });

        Elements rows;
        rows.push_back(col_header);
        rows.push_back(separatorDouble());

        for (int i = 0; i < (int)downloads.size(); ++i) {
            bool sel = (i == selected);
            auto row_el = render_dl_row(downloads[i], sel);
            rows.push_back(sel ? (row_el | focus) : row_el);
        }

        body = vbox(std::move(rows)) | vscroll_indicator | yframe | flex;
    }

    auto footer = hbox({
        text(" [TAB]") | color(Color::Cyan) | bold,
        text(" Switch") | color(Color::GrayDark),
        text("   [") | color(Color::GrayDark),
        text("\xe2\x86\x91\xe2\x86\x93") | color(Color::Cyan) | bold,
        text("] Navigate") | color(Color::GrayDark),
        text("   [Q]") | color(Color::Cyan) | bold,
        text(" Quit") | color(Color::GrayDark),
    });

    return vbox({
        header,
        separator(),
        body,
        separator(),
        footer,
    }) | borderDouble | color(Color::Cyan);
}

// ── Search screen UI builder ──────────────────────────────────────────────────

static Element render_result_row(const SearchResult &r, bool selected, bool queued)
{
    char size_buf[32];
    fmt_bytes(r.size, size_buf, sizeof(size_buf));

    auto row = hbox({
        text(selected ? " \xe2\x96\xba " : "   ") | color(Color::Cyan),
        text(r.name) | color(Color::White) | flex,
        text(" "),
        text(std::to_string(r.seeders))  | color(Color::Green)  | size(WIDTH, EQUAL, 7),
        text(" "),
        text(std::to_string(r.leechers)) | color(Color::Red)    | size(WIDTH, EQUAL, 7),
        text(" "),
        text(size_buf)                   | color(Color::Yellow) | size(WIDTH, EQUAL, 10),
    });

    if (selected) return row | inverted;
    if (queued)   return row | color(Color::MagentaLight);
    return row;
}

static const char *SPINNER_FRAMES[] = {
    "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
    "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7"
};
static constexpr int SPINNER_COUNT = 8;

static Element build_search_ui(const SearchState &s, Element input_el)
{
    auto header = hbox({
        text(" \xe2\x98\x85 TORRENT-DL v1.0 \xe2\x98\x85 ") | bold | color(Color::Cyan),
    }) | hcenter;

    auto input_row = hbox({
        text(" SEARCH: ") | color(Color::Cyan) | bold,
        std::move(input_el) | flex,
        text(" "),
    });

    Element body;

    if (s.status == SearchStatus::IDLE) {
        body = vbox({
            filler(),
            hbox({
                filler(),
                text("  \xe2\x97\x86 Type a query and press") | color(Color::GrayDark),
                text(" [ENTER]") | color(Color::Cyan) | bold,
                text(" to search  \xe2\x97\x86") | color(Color::GrayDark),
                filler(),
            }),
            filler(),
        }) | flex;
    } else if (s.status == SearchStatus::LOADING) {
        size_t frame = s.spinner_frame % SPINNER_COUNT;
        body = vbox({
            filler(),
            hbox({
                filler(),
                text("  ") | color(Color::Cyan),
                text(SPINNER_FRAMES[frame]) | color(Color::Cyan) | bold,
                text("  Searching...") | color(Color::Yellow) | bold,
                filler(),
            }),
            filler(),
        }) | flex;
    } else if (s.status == SearchStatus::ERR) {
        body = vbox({
            filler(),
            hbox({
                filler(),
                text("  \xe2\x9c\x96 ERROR: " + s.error_msg + "  ") | color(Color::Red),
                filler(),
            }),
            filler(),
        }) | flex;
    } else {
        // DONE
        if (s.results.empty()) {
            body = vbox({
                filler(),
                hbox({
                    filler(),
                    text("  No results found.  ") | color(Color::GrayDark),
                    filler(),
                }),
                filler(),
            }) | flex;
        } else {
            auto col_header = hbox({
                text("   ") | color(Color::Cyan),
                text("NAME") | color(Color::Cyan) | bold | flex,
                text(" "),
                text("SEEDS") | color(Color::Cyan) | bold | size(WIDTH, EQUAL, 7),
                text(" "),
                text("PEERS") | color(Color::Cyan) | bold | size(WIDTH, EQUAL, 7),
                text(" "),
                text("SIZE    ") | color(Color::Cyan) | bold | size(WIDTH, EQUAL, 10),
            });

            Elements rows;
            rows.push_back(col_header);
            rows.push_back(separatorDouble());

            for (int i = 0; i < (int)s.results.size(); ++i) {
                bool sel    = (i == s.selected);
                bool queued = g_queued_hashes.count(s.results[i].info_hash) > 0;
                auto row_el = render_result_row(s.results[i], sel, queued);
                rows.push_back(sel ? (row_el | focus) : row_el);
            }

            body = vbox(std::move(rows)) | vscroll_indicator | yframe | flex;
        }
    }

    auto footer = hbox({
        text(" [ENTER]") | color(Color::Cyan) | bold,
        text(" Search/Select") | color(Color::GrayDark),
        text("   [") | color(Color::GrayDark),
        text("\xe2\x86\x91\xe2\x86\x93") | color(Color::Cyan) | bold,
        text("] Navigate") | color(Color::GrayDark),
        text("   [TAB]") | color(Color::Cyan) | bold,
        text(" Switch") | color(Color::GrayDark),
        text("   [Q]") | color(Color::Cyan) | bold,
        text(" Quit") | color(Color::GrayDark),
    });

    return vbox({
        header,
        separator(),
        input_row,
        separator(),
        body,
        separator(),
        footer,
    }) | borderDouble | color(Color::Cyan);
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
        lt::settings_pack sp;
        sp.set_int(lt::settings_pack::alert_mask,
            lt::alert_category::error   |
            lt::alert_category::status  |
            lt::alert_category::storage);
        sp.set_str(lt::settings_pack::listen_interfaces,
            "0.0.0.0:" + std::to_string(g_listen_port));

        lt::session ses(sp);
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

    // Create shared libtorrent session up front
    {
        lt::settings_pack sp;
        sp.set_int(lt::settings_pack::alert_mask,
            lt::alert_category::error   |
            lt::alert_category::status  |
            lt::alert_category::storage);
        sp.set_str(lt::settings_pack::listen_interfaces,
            "0.0.0.0:" + std::to_string(g_listen_port));
        g_ses = new lt::session(sp);
    }

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

        // Pre-allocate the handle slot so the index is always in sync
        {
            std::lock_guard<std::mutex> lk(g_handles_mutex);
            g_handles.emplace_back();  // placeholder, replaced once add_torrent succeeds
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

            // Store handle at the reserved slot — dispatch thread matches by handle
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
        // Q to quit (only when input is empty to avoid eating 'q' while typing)
        if (e == Event::Character('q') || e == Event::Character('Q')) {
            if (search_input_content.empty()) {
                g_quit = true;
                screen.ExitLoopClosure()();
                return true;
            }
            return false;
        }

        // Tab / Shift-Tab to switch tabs
        if (e == Event::Tab || e == Event::TabReverse) {
            g_tab_index = (g_tab_index + 1) % 2;
            return true;
        }

        // Enter: select result (if DONE and non-empty) or trigger search
        if (e == Event::Return) {
            if (g_search.status == SearchStatus::DONE && !g_search.results.empty()) {
                const auto &r = g_search.results[g_search.selected];
                std::string magnet = "magnet:?xt=urn:btih:" + r.info_hash
                    + "&dn=" + url_encode(r.name)
                    + "&tr=udp://tracker.opentrackr.org:1337/announce"
                    + "&tr=udp://tracker.openbittorrent.com:6969/announce"
                    + "&tr=udp://open.demonii.com:1337/announce";
                start_download(magnet, r.info_hash, r.name);
                screen.Post(Event::Custom);
                return true;
            }
            trigger_search(screen, search_input_content);
            return true;
        }

        // Arrow navigation for results list
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
