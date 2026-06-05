#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>
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

// Download state — only written via screen.Post() lambdas
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

// Search state
struct SearchResult {
    std::string id, name, info_hash, category;
    int     seeders  = 0;
    int     leechers = 0;
    int64_t size     = 0;
};

enum class SearchStatus { IDLE, LOADING, DONE, ERR };

struct SearchState {
    std::string              query;
    std::vector<SearchResult> results;
    int                      selected      = 0;
    SearchStatus             status        = SearchStatus::IDLE;
    std::string              error_msg;
    size_t                   spinner_frame = 0;
};

static SearchState  g_search;
static int          g_tab_index       = 0;   // 0 = search, 1 = download
static std::string  g_selected_magnet;
static std::string  g_save_path       = ".";
static int          g_listen_port     = 6881;

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

static std::string fmt_eta(int64_t remaining, double rate)
{
    if (rate < 1.0 || remaining <= 0) return "--:--";
    long long s = static_cast<long long>(remaining / rate);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lldm %02llds", s / 60, s % 60);
    return buf;
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

    g_search.status  = SearchStatus::LOADING;
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

// ── libtorrent polling loop (shared between direct-start and search-start) ────

static void run_lt_loop(ScreenInteractive &screen, lt::session &ses,
                         lt::torrent_handle h)
{
    while (!g_quit) {
        std::vector<lt::alert *> alerts;
        ses.pop_alerts(&alerts);

        for (lt::alert *a : alerts) {
            if (lt::alert_cast<lt::torrent_error_alert>(a)) {
                screen.Post([] { g_state.state_label = "ERROR"; });
            } else if (lt::alert_cast<lt::torrent_finished_alert>(a)) {
                screen.Post([] {
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
        screen.Post([st] {
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
}

// ── Download screen UI builder ────────────────────────────────────────────────

static Element build_dl_ui(const TorrentState &s)
{
    const int bar_width = 24;
    int filled = static_cast<int>(s.progress * bar_width);
    std::string bar_str;
    for (int i = 0; i < bar_width; ++i)
        bar_str += (i < filled) ? "\xe2\x96\x88" : "\xe2\x96\x91";

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

// ── Search screen UI builder ──────────────────────────────────────────────────

static Element render_result_row(const SearchResult &r, bool selected)
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
            // Column headers
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
                bool sel = (i == s.selected);
                auto row_el = render_result_row(s.results[i], sel);
                if (sel)
                    rows.push_back(row_el | focus);
                else
                    rows.push_back(row_el);
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
            atp.ti = ti;
            g_state.name = ti->name();
        }

        lt::torrent_handle h = ses.add_torrent(std::move(atp), ec);
        if (ec) {
            fprintf(stderr, "Failed to add torrent: %s\n", ec.message().c_str());
            return 1;
        }

        auto renderer = Renderer([&] { return build_dl_ui(g_state); });
        auto ui = CatchEvent(renderer, [&](Event e) {
            if (e == Event::Character('q') || e == Event::Character('Q')) {
                g_quit = true;
                screen.ExitLoopClosure()();
                return true;
            }
            return false;
        });

        std::thread lt_thread([&] { run_lt_loop(screen, ses, h); });
        screen.Loop(ui);
        g_quit = true;
        lt_thread.join();
        h.save_resume_data();
        ses.pause();
        return 0;
    }

    // ── Search-first mode ─────────────────────────────────────────────────────

    // libtorrent session is heap-allocated when download starts
    lt::session *g_ses = nullptr;

    std::string search_input_content;

    InputOption input_opt;
    input_opt.multiline = false;

    auto search_input = Input(&search_input_content, "Search torrents...", input_opt);

    // Download screen component (starts invisible — Container::Tab hides it)
    auto dl_renderer = Renderer([&] { return build_dl_ui(g_state); });
    auto dl_component = CatchEvent(dl_renderer, [&](Event e) {
        if (e == Event::Character('q') || e == Event::Character('Q')) {
            g_quit = true;
            screen.ExitLoopClosure()();
            return true;
        }
        return false;
    });

    // start_download — called when user selects a result
    auto start_download = [&] {
        std::thread([&screen, &g_ses] {
            lt::settings_pack sp;
            sp.set_int(lt::settings_pack::alert_mask,
                lt::alert_category::error   |
                lt::alert_category::status  |
                lt::alert_category::storage);
            sp.set_str(lt::settings_pack::listen_interfaces,
                "0.0.0.0:" + std::to_string(g_listen_port));

            g_ses = new lt::session(sp);

            lt::error_code ec;
            lt::add_torrent_params atp = lt::parse_magnet_uri(g_selected_magnet, ec);
            if (ec) {
                screen.Post([] { g_state.state_label = "ERROR"; });
                screen.Post(Event::Custom);
                return;
            }
            atp.save_path = g_save_path;

            lt::torrent_handle h = g_ses->add_torrent(std::move(atp), ec);
            if (ec) {
                screen.Post([] { g_state.state_label = "ERROR"; });
                screen.Post(Event::Custom);
                return;
            }

            // Switch to download view
            screen.Post([] { g_tab_index = 1; });
            screen.Post(Event::Custom);

            run_lt_loop(screen, *g_ses, h);
        }).detach();
    };

    // Search screen component
    auto search_renderer = Renderer(search_input, [&] {
        return build_search_ui(g_search, search_input->Render());
    });

    auto search_component = CatchEvent(search_renderer, [&](Event e) {
        // Q to quit
        if (e == Event::Character('q') || e == Event::Character('Q')) {
            // only quit from search screen if input is empty (avoid eating 'q' while typing)
            if (search_input_content.empty()) {
                g_quit = true;
                screen.ExitLoopClosure()();
                return true;
            }
            return false;
        }

        // Enter: either search (if IDLE/DONE/ERR) or select result (if DONE and non-empty)
        if (e == Event::Return) {
            if (g_search.status == SearchStatus::DONE && !g_search.results.empty()) {
                // Select current result
                const auto &r = g_search.results[g_search.selected];
                g_selected_magnet = "magnet:?xt=urn:btih:" + r.info_hash
                    + "&dn=" + url_encode(r.name)
                    + "&tr=udp://tracker.opentrackr.org:1337/announce"
                    + "&tr=udp://tracker.openbittorrent.com:6969/announce"
                    + "&tr=udp://open.demonii.com:1337/announce";
                g_state.name = r.name;
                start_download();
                return true;
            }
            // Otherwise let the Input's on_enter fire (handled below)
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

    // Root: Tab between search and download
    auto root = Container::Tab({search_component, dl_component}, &g_tab_index);

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

    screen.Loop(root);

    g_quit = true;
    spinner_thread.join();

    if (g_ses) {
        g_ses->pause();
        delete g_ses;
    }

    return 0;
}
