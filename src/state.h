#pragma once

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <libtorrent/torrent_handle.hpp>

namespace lt = libtorrent;

// ── Types ─────────────────────────────────────────────────────────────────────

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

// ── Global state ──────────────────────────────────────────────────────────────

extern std::atomic<bool>               g_quit;
extern std::vector<DownloadEntry>      g_downloads;
extern int                             g_dl_selected;
extern std::set<std::string>           g_queued_hashes;
extern lt::session                    *g_ses;
extern std::vector<lt::torrent_handle> g_handles;
extern std::mutex                      g_handles_mutex;
extern SearchState                     g_search;
extern int                             g_tab_index;
extern std::string                     g_selected_magnet;
extern std::string                     g_save_path;
extern int                             g_listen_port;
