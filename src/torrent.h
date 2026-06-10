#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_status.hpp>

#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/component/event.hpp"

#include "state.h"

namespace lt = libtorrent;
using namespace ftxui;

// Tracker list used when building magnet URIs from search results
static const char *const TRACKERS[] = {
    "udp://tracker.opentrackr.org:1337/announce",
    "udp://tracker.openbittorrent.com:6969/announce",
    "udp://open.demonii.com:1337/announce",
};
static constexpr int TRACKER_COUNT = 3;

// Shared libtorrent session settings
static inline lt::settings_pack make_session_settings(int listen_port)
{
    lt::settings_pack sp;
    sp.set_int(lt::settings_pack::alert_mask,
        lt::alert_category::error   |
        lt::alert_category::status  |
        lt::alert_category::storage);
    sp.set_str(lt::settings_pack::listen_interfaces,
        "0.0.0.0:" + std::to_string(listen_port));
    return sp;
}

// ── libtorrent alert dispatch loop (single thread handles all torrents) ───────

static inline void run_lt_dispatch(ScreenInteractive &screen, lt::session &ses)
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
                    if (g_downloads[idx].name.empty() && !st.name.empty())
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
