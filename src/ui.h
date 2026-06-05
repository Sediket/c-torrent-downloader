#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"
#include "ftxui/component/component.hpp"

#include "state.h"
#include "formatters.h"

using namespace ftxui;

// ── Downloads list UI ─────────────────────────────────────────────────────────

static inline Element render_dl_row(const DownloadEntry &e, bool selected)
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

static inline Element build_dl_list_ui(const std::vector<DownloadEntry> &downloads, int selected)
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
        text("   [CTRL+C]") | color(Color::Cyan) | bold,
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

// ── Search screen UI ──────────────────────────────────────────────────────────

static inline Element render_result_row(const SearchResult &r, bool selected, bool queued)
{
    char size_buf[32];
    fmt_bytes(r.size, size_buf, sizeof(size_buf));

    std::string marker = selected ? " \xe2\x96\xba " : (queued ? " \xe2\x86\x93 " : "   ");
    Color marker_col   = selected ? Color::Cyan : Color::MagentaLight;
    Color name_col     = queued   ? Color::MagentaLight : Color::White;

    auto row = hbox({
        text(marker) | color(marker_col),
        text(r.name) | color(name_col) | flex,
        text(" "),
        text(std::to_string(r.seeders))  | color(queued ? Color::MagentaLight : Color::Green)  | size(WIDTH, EQUAL, 7),
        text(" "),
        text(std::to_string(r.leechers)) | color(queued ? Color::MagentaLight : Color::Red)    | size(WIDTH, EQUAL, 7),
        text(" "),
        text(size_buf)                   | color(queued ? Color::MagentaLight : Color::Yellow) | size(WIDTH, EQUAL, 10),
    });

    if (selected) return row | inverted;
    return row;
}

static const char *SPINNER_FRAMES[] = {
    "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
    "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7"
};
static constexpr int SPINNER_COUNT = 8;

static inline Element build_search_ui(const SearchState &s, Element input_el)
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
        text("   [CTRL+C]") | color(Color::Cyan) | bold,
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
