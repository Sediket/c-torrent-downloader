#pragma once

#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

#include <nlohmann/json.hpp>

#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/component/event.hpp"

#include "state.h"
#include "formatters.h"

using namespace ftxui;

// ── WinHTTP GET ───────────────────────────────────────────────────────────────

#ifdef _WIN32
static inline std::string winhttp_get(const std::wstring &host, const std::wstring &path)
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

static inline void trigger_search(ScreenInteractive &screen, const std::string &query)
{
    if (query.empty()) return;
    if (g_search.status == SearchStatus::LOADING) return;

    g_search.status        = SearchStatus::LOADING;
    g_search.results.clear();
    g_search.selected      = 0;
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
