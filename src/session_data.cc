/**
 * Copyright (c) 2013, Timothy Stack
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of Timothy Stack nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @file session_data.cc
 */

#include "config.h"

#include <stdio.h>
#include <glob.h>
#include <fcntl.h>
#include <sys/types.h>
#include <dirent.h>

#include "spookyhash/SpookyV2.h"

#include <algorithm>
#include <utility>
#include <yajl/api/yajl_tree.h>

#include "yajlpp/yajlpp.hh"
#include "yajlpp/yajlpp_def.hh"
#include "lnav.hh"
#include "logfile.hh"
#include "sql_util.hh"
#include "lnav_util.hh"
#include "lnav_config.hh"
#include "session_data.hh"
#include "command_executor.hh"

using namespace std;

static const char *LOG_METADATA_NAME = "log_metadata.db";

static const char *BOOKMARK_TABLE_DEF = R"(
CREATE TABLE IF NOT EXISTS bookmarks (
    log_time datetime,
    log_format varchar(64),
    log_hash varchar(128),
    session_time integer,
    part_name text,
    access_time datetime DEFAULT CURRENT_TIMESTAMP,
    comment text DEFAULT '',
    tags text DEFAULT '',

    PRIMARY KEY (log_time, log_format, log_hash, session_time)
);

CREATE TABLE IF NOT EXISTS time_offset (
    log_time datetime,
    log_format varchar(64),
    log_hash varchar(128),
    session_time integer,
    offset_sec integer,
    offset_usec integer,
    access_time datetime DEFAULT CURRENT_TIMESTAMP,

    PRIMARY KEY (log_time, log_format, log_hash, session_time)
);
)";

static const char *BOOKMARK_LRU_STMT =
    "DELETE FROM bookmarks WHERE access_time <= "
    "  (SELECT access_time FROM bookmarks "
    "   ORDER BY access_time DESC LIMIT 1 OFFSET 50000)";

static const char *UPGRADE_STMTS[] = {
    R"(ALTER TABLE bookmarks ADD COLUMN comment text DEFAULT '';)",
    R"(ALTER TABLE bookmarks ADD COLUMN tags text DEFAULT '';)",
};

static const size_t MAX_SESSIONS           = 8;
static const size_t MAX_SESSION_FILE_COUNT = 256;

static std::vector<content_line_t> marked_session_lines;
static std::vector<content_line_t> offset_session_lines;

int bind_to_sqlite(sqlite3_stmt *stmt, int index, const struct timeval &tv)
{
    char timestamp[64];

    sql_strftime(timestamp, sizeof(timestamp), tv, 'T');

    return sqlite3_bind_text(stmt, index, timestamp, -1, SQLITE_TRANSIENT);
}

int bind_to_sqlite(sqlite3_stmt *stmt, int index, const char *str)
{
    return sqlite3_bind_text(stmt, index, str, -1, SQLITE_TRANSIENT);
}

int bind_to_sqlite(sqlite3_stmt *stmt, int index, intern_string_t ist)
{
    return sqlite3_bind_text(stmt, index, ist.get(), ist.size(), SQLITE_TRANSIENT);
}

int bind_to_sqlite(sqlite3_stmt *stmt, int index, const string &str)
{
    return sqlite3_bind_text(stmt, index, str.c_str(), str.size(), SQLITE_TRANSIENT);
}

int bind_to_sqlite(sqlite3_stmt *stmt, int index, int64_t i)
{
    return sqlite3_bind_int64(stmt, index, i);
}

template <typename... Args, std::size_t... Idx>
int bind_values_helper(sqlite3_stmt *stmt, std::index_sequence<Idx...> idxs, Args ... args) {
    int rcs[] = { bind_to_sqlite(stmt, Idx + 1, args)... };

    for (int lpc = 0; lpc < idxs.size(); lpc++) {
        if (rcs[lpc] != SQLITE_OK) {
            log_error("Failed to bind column %d in statement: %s", lpc, sqlite3_sql(stmt));
            return rcs[lpc];
        }
    }

    return SQLITE_OK;
}

template <typename... Args>
int bind_values(sqlite3_stmt *stmt, Args ... args) {
    return bind_values_helper(stmt, std::make_index_sequence<sizeof...(Args)>(), args...);
}

static bool bind_line(sqlite3 *db,
                      sqlite3_stmt *stmt,
                      content_line_t cl,
                      time_t session_time)
{
    logfile_sub_source &lss = lnav_data.ld_log_source;
    shared_ptr<logfile> lf;

    lf = lss.find(cl);

    if (lf == nullptr) {
        return false;
    }

    sqlite3_clear_bindings(stmt);

    auto line_iter = lf->begin() + cl;
    auto read_result = lf->read_line(line_iter);

    if (read_result.isErr()) {
        return false;
    }

    auto line_hash = read_result.map([cl](auto sbr) {
        return hash_bytes(sbr.get_data(), sbr.length(),
                          &cl, sizeof(cl),
                          nullptr);
    }).unwrap();

    return bind_values(stmt,
                       lf->original_line_time(line_iter),
                       lf->get_format()->get_name(),
                       line_hash,
                       session_time) == SQLITE_OK;
}

struct session_file_info {
    session_file_info(int timestamp,
                      string id,
                      string path)
        : sfi_timestamp(timestamp),
          sfi_id(std::move(id)),
          sfi_path(std::move(path)) {
    };

    bool operator<(const session_file_info &other) const
    {
        if (this->sfi_timestamp < other.sfi_timestamp) {
            return true;
        }
        if (this->sfi_path < other.sfi_path) {
            return true;
        }
        return false;
    };

    int    sfi_timestamp;
    string sfi_id;
    string sfi_path;
};

static void cleanup_session_data()
{
    static_root_mem<glob_t, globfree>   session_file_list;
    std::list<struct session_file_info> session_info_list;
    map<string, int> session_count;
    auto session_file_pattern = dotlnav_path() / "*-*.ts*.json";

    if (glob(session_file_pattern.str().c_str(),
             0,
             nullptr,
             session_file_list.inout()) == 0) {
        for (size_t lpc = 0; lpc < session_file_list->gl_pathc; lpc++) {
            const char *path = session_file_list->gl_pathv[lpc];
            char        hash_id[64];
            int         timestamp;
            const char *base;

            base = strrchr(path, '/');
            if (base == nullptr) {
                continue;
            }
            base += 1;
            if (sscanf(base, "file-%63[^.].ts%d.json",
                       hash_id, &timestamp) == 2) {
                session_count[hash_id] += 1;
                session_info_list.emplace_back(timestamp, hash_id, path);
            }
            if (sscanf(base,
                       "view-info-%63[^.].ts%d.ppid%*d.json",
                       hash_id,
                       &timestamp) == 2) {
                session_count[hash_id] += 1;
                session_info_list.emplace_back(timestamp, hash_id, path);
            }
        }
    }

    session_info_list.sort();

    size_t session_loops = 0;

    while (session_info_list.size() > MAX_SESSION_FILE_COUNT) {
        const session_file_info &front = session_info_list.front();

        session_loops += 1;
        if (session_loops < MAX_SESSION_FILE_COUNT &&
            session_count[front.sfi_id] == 1) {
            session_info_list.splice(session_info_list.end(),
                                     session_info_list,
                                     session_info_list.begin());
        }
        else {
            if (remove(front.sfi_path.c_str()) != 0) {
                log_error(
                        "Unable to remove session file: %s -- %s\n",
                        front.sfi_path.c_str(),
                        strerror(errno));
            }
            session_count[front.sfi_id] -= 1;
            session_info_list.pop_front();
        }
    }

    session_info_list.sort();

    while (session_info_list.size() > MAX_SESSION_FILE_COUNT) {
        const session_file_info &front = session_info_list.front();

        if (remove(front.sfi_path.c_str()) != 0) {
            log_error(
                    "Unable to remove session file: %s -- %s\n",
                    front.sfi_path.c_str(),
                    strerror(errno));
        }
        session_count[front.sfi_id] -= 1;
        session_info_list.pop_front();
    }
}

void init_session()
{
    lnav_data.ld_session_time = time(nullptr);
    lnav_data.ld_session_id.clear();
}

static nonstd::optional<std::string> compute_session_id()
{
    byte_array<2, uint64> hash;
    SpookyHash context;
    bool has_files = false;

    context.Init(0, 0);
    hash_updater updater(&context);
    for (auto &ld_file_name : lnav_data.ld_file_names) {
        if (!ld_file_name.second.loo_include_in_session) {
            continue;
        }
        has_files = true;
        updater(ld_file_name.first);
    }
    if (!has_files) {
        return nonstd::nullopt;
    }
    context.Final(hash.out(0), hash.out(1));

    return hash.to_string();
}

nonstd::optional<session_pair_t> scan_sessions()
{
    static_root_mem<glob_t, globfree> view_info_list;
    char view_info_pattern_base[128];

    cleanup_session_data();

    const auto session_id = compute_session_id();
    if (!session_id) {
        return nonstd::nullopt;
    }
    std::list<session_pair_t> &session_file_names =
        lnav_data.ld_session_id[session_id.value()];

    session_file_names.clear();

    snprintf(view_info_pattern_base, sizeof(view_info_pattern_base),
             "view-info-%s.*.json",
             session_id.value().c_str());
    auto view_info_pattern = dotlnav_path() / view_info_pattern_base;
    if (glob(view_info_pattern.str().c_str(), 0, nullptr,
             view_info_list.inout()) == 0) {
        for (size_t lpc = 0; lpc < view_info_list->gl_pathc; lpc++) {
            const char *path = view_info_list->gl_pathv[lpc];
            int timestamp, ppid, rc;
            const char *base;

            base = strrchr(path, '/');
            if (base == nullptr) {
                continue;
            }
            base += 1;
            if ((rc = sscanf(base,
                             "view-info-%*[^.].ts%d.ppid%d.json",
                             &timestamp,
                             &ppid)) == 2) {
                ppid_time_pair_t ptp;

                ptp.first  = (ppid == getppid()) ? 1 : 0;
                ptp.second = timestamp;
                session_file_names.emplace_back(ptp, path);
            }
        }
    }

    session_file_names.sort();

    while (session_file_names.size() > MAX_SESSIONS) {
        const std::string &name = session_file_names.front().second;

        if (remove(name.c_str()) != 0) {
            log_error(
                    "Unable to remove session: %s -- %s\n",
                    name.c_str(),
                    strerror(errno));
        }
        session_file_names.pop_front();
    }

    if (session_file_names.empty()) {
        return nonstd::nullopt;
    }

    return nonstd::make_optional(session_file_names.back());
}

static void load_time_bookmarks()
{
    logfile_sub_source &lss = lnav_data.ld_log_source;
    std::map<content_line_t, bookmark_metadata> &bm_meta = lss.get_user_bookmark_metadata();
    auto_mem<sqlite3, sqlite_close_wrapper> db;
    auto db_path = dotlnav_path() / LOG_METADATA_NAME;
    auto_mem<sqlite3_stmt> stmt(sqlite3_finalize);
    logfile_sub_source::iterator file_iter;
    bool reload_needed = false;
    auto_mem<char, sqlite3_free> errmsg;

    log_info("loading bookmark db: %s", db_path.str().c_str());

    if (sqlite3_open(db_path.str().c_str(), db.out()) != SQLITE_OK) {
        return;
    }

    for (const char *upgrade_stmt : UPGRADE_STMTS) {
        if (sqlite3_exec(db.in(), upgrade_stmt, nullptr, nullptr, errmsg.out()) != SQLITE_OK) {
            log_error("unable to upgrade bookmark table -- %s\n", errmsg.in());
        }
    }

    if (sqlite3_prepare_v2(db.in(),
                           "SELECT log_time, log_format, log_hash, session_time, part_name, access_time, comment,"
                           " tags, session_time=? as same_session FROM bookmarks WHERE "
                           " log_time between ? and ? and log_format = ? "
                           " ORDER BY same_session DESC, session_time DESC",
                           -1,
                           stmt.out(),
                           nullptr) != SQLITE_OK) {
        log_error(
                "could not prepare bookmark select statement -- %s\n",
                sqlite3_errmsg(db));
        return;
    }

    for (file_iter = lnav_data.ld_log_source.begin();
         file_iter != lnav_data.ld_log_source.end();
         ++file_iter) {
        shared_ptr<logfile> lf = (*file_iter)->get_file();
        content_line_t base_content_line;

        if (lf == nullptr) {
            continue;
        }

        base_content_line = lss.get_file_base_content_line(file_iter);

        auto low_line_iter = lf->begin();
        auto high_line_iter = lf->end();

        --high_line_iter;

        if (bind_values(stmt.in(),
                        lnav_data.ld_session_load_time,
                        lf->original_line_time(low_line_iter),
                        lf->original_line_time(high_line_iter),
                        lf->get_format()->get_name()) != SQLITE_OK) {
            return;
        }

        date_time_scanner dts;
        bool done = false;
        string line;
        int64_t last_mark_time = -1;

        while (!done) {
            int rc = sqlite3_step(stmt.in());

            switch (rc) {
            case SQLITE_OK:
            case SQLITE_DONE:
                done = true;
                break;

            case SQLITE_ROW: {
                const char *log_time = (const char *)sqlite3_column_text(stmt.in(), 0);
                const char *log_hash = (const char *)sqlite3_column_text(stmt.in(), 2);
                const char *part_name = (const char *)sqlite3_column_text(stmt.in(), 4);
                const char *comment = (const char *)sqlite3_column_text(stmt.in(), 6);
                const char *tags = (const char *)sqlite3_column_text(stmt.in(), 7);
                int64_t mark_time = sqlite3_column_int64(stmt.in(), 3);
                struct timeval log_tv;
                struct exttm log_tm;

                if (last_mark_time == -1) {
                    last_mark_time = mark_time;
                }
                else if (last_mark_time != mark_time) {
                    done = true;
                    continue;
                }

                if (part_name == nullptr) {
                    continue;
                }

                if (!dts.scan(log_time, strlen(log_time), NULL, &log_tm, log_tv)) {
                    continue;
                }

                auto line_iter = lower_bound(lf->begin(), lf->end(), log_tv);
                while (line_iter != lf->end()) {
                    struct timeval line_tv = line_iter->get_timeval();

                    if ((line_tv.tv_sec != log_tv.tv_sec) ||
                        (line_tv.tv_usec != log_tv.tv_usec)) {
                        break;
                    }

                    content_line_t cl = content_line_t(std::distance(lf->begin(), line_iter));
                    auto read_result = lf->read_line(line_iter);

                    if(read_result.isErr()) {
                        break;
                    }

                    auto sbr = read_result.unwrap();

                    string line_hash = hash_bytes(sbr.get_data(), sbr.length(),
                                                  &cl, sizeof(cl),
                                                  nullptr);

                    if (line_hash == log_hash) {
                        content_line_t line_cl = content_line_t(
                            base_content_line + std::distance(lf->begin(), line_iter));
                        bool meta = false;

                        if (part_name != nullptr && part_name[0] != '\0') {
                            lss.set_user_mark(&textview_curses::BM_META, line_cl);
                            bm_meta[line_cl].bm_name = part_name;
                            meta = true;
                        }
                        if (comment != nullptr && comment[0] != '\0') {
                            lss.set_user_mark(&textview_curses::BM_META,
                                              line_cl);
                            bm_meta[line_cl].bm_comment = comment;
                            meta = true;
                        }
                        if (tags != nullptr && tags[0] != '\0') {
                            auto_mem<yajl_val_s> tag_list(yajl_tree_free);
                            char error_buffer[1024];

                            tag_list = yajl_tree_parse(tags, error_buffer, sizeof(error_buffer));
                            if (!YAJL_IS_ARRAY(tag_list.in())) {
                                log_error("invalid tags column: %s", tags);
                            } else {
                                lss.set_user_mark(&textview_curses::BM_META,
                                                  line_cl);
                                for (int lpc = 0; lpc < tag_list.in()->u.array.len; lpc++) {
                                    yajl_val elem = tag_list.in()->u.array.values[lpc];

                                    if (!YAJL_IS_STRING(elem)) {
                                        continue;
                                    }
                                    bookmark_metadata::KNOWN_TAGS.insert(elem->u.string);
                                    bm_meta[line_cl].add_tag(elem->u.string);
                                }
                            }
                            meta = true;
                        }
                        if (!meta) {
                            marked_session_lines.push_back(line_cl);
                            lss.set_user_mark(&textview_curses::BM_USER,
                                              line_cl);
                        }
                        reload_needed = true;
                    }

                    ++line_iter;
                }
                break;
            }

            default:
                {
                    const char *errmsg;

                    errmsg = sqlite3_errmsg(lnav_data.ld_db);
                    log_error(
                            "bookmark select error: code %d -- %s\n",
                            rc,
                            errmsg);
                    done = true;
                }
                break;
            }
        }

        sqlite3_reset(stmt.in());
    }

    if (sqlite3_prepare_v2(db.in(),
                           "SELECT *,session_time=? as same_session FROM time_offset WHERE "
                           " log_time between ? and ? and log_format = ? "
                           " ORDER BY same_session DESC, session_time DESC",
                           -1,
                           stmt.out(),
                           nullptr) != SQLITE_OK) {
        log_error(
                "could not prepare time_offset select statement -- %s\n",
                sqlite3_errmsg(db));
        return;
    }

    for (file_iter = lnav_data.ld_log_source.begin();
         file_iter != lnav_data.ld_log_source.end();
         ++file_iter) {
        shared_ptr<logfile> lf = (*file_iter)->get_file();
        content_line_t base_content_line;

        if (lf == nullptr) {
            continue;
        }

        lss.find(lf->get_filename().c_str(), base_content_line);

        auto low_line_iter = lf->begin();
        auto high_line_iter = lf->end();

        --high_line_iter;

        if (bind_values(stmt.in(),
                        lnav_data.ld_session_load_time,
                        lf->original_line_time(low_line_iter),
                        lf->original_line_time(high_line_iter),
                        lf->get_format()->get_name()) != SQLITE_OK) {
            return;
        }

        date_time_scanner dts;
        bool done = false;
        string line;
        int64_t last_mark_time = -1;

        while (!done) {
            int rc = sqlite3_step(stmt.in());

            switch (rc) {
            case SQLITE_OK:
            case SQLITE_DONE:
                done = true;
                break;

            case SQLITE_ROW: {
                const char *log_time = (const char *)sqlite3_column_text(stmt.in(), 0);
                const char *log_hash = (const char *)sqlite3_column_text(stmt.in(), 2);
                int64_t mark_time = sqlite3_column_int64(stmt.in(), 3);
                struct timeval log_tv;
                struct exttm log_tm;

                if (last_mark_time == -1) {
                    last_mark_time = mark_time;
                }
                else if (last_mark_time != mark_time) {
                    done = true;
                    continue;
                }

                if (sqlite3_column_type(stmt.in(), 4) == SQLITE_NULL) {
                    continue;
                }

                if (!dts.scan(log_time, strlen(log_time), nullptr, &log_tm, log_tv)) {
                    continue;
                }

                auto line_iter = lower_bound(lf->begin(),
                                             lf->end(),
                                             log_tv);
                while (line_iter != lf->end()) {
                    struct timeval line_tv = line_iter->get_timeval();

                    if ((line_tv.tv_sec != log_tv.tv_sec) ||
                        (line_tv.tv_usec != log_tv.tv_usec)) {
                        break;
                    }

                    auto read_result = lf->read_line(line_iter);

                    if (read_result.isErr()) {
                        break;
                    }

                    auto sbr = read_result.unwrap();

                    string line_hash = hash_bytes(sbr.get_data(), sbr.length(), nullptr);
                    if (line_hash == log_hash) {
                        int file_line = std::distance(lf->begin(), line_iter);
                        content_line_t line_cl = content_line_t(
                            base_content_line + file_line);
                        struct timeval offset;

                        offset_session_lines.push_back(line_cl);
                        offset.tv_sec = sqlite3_column_int64(stmt.in(), 4);
                        offset.tv_usec = sqlite3_column_int64(stmt.in(), 5);
                        lf->adjust_content_time(file_line, offset);

                        reload_needed = true;
                    }

                    ++line_iter;
                }
                break;
            }

            default:
                {
                    const char *errmsg;

                    errmsg = sqlite3_errmsg(lnav_data.ld_db);
                    log_error(
                            "bookmark select error: code %d -- %s\n",
                            rc,
                            errmsg);
                    done = true;
                }
                break;
            }
        }

        sqlite3_reset(stmt.in());
    }

    if (reload_needed) {
        lnav_data.ld_views[LNV_LOG].reload_data();
    }
}

static int read_save_time(yajlpp_parse_context *ypc, long long value)
{
    lnav_data.ld_session_save_time = value;

    return 1;
}

static int read_time_offset(yajlpp_parse_context *ypc, int value)
{
    return 1;
}

static int read_files(yajlpp_parse_context *ypc, const unsigned char *str, size_t len)
{
    return 1;
}

static int read_last_search(yajlpp_parse_context *ypc, const unsigned char *str, size_t len)
{
    string       regex        = std::string((const char *)str, len);
    const char **view_name;
    int          view_index;

    view_name = find(lnav_view_strings,
                     lnav_view_strings + LNV__MAX,
                     ypc->get_path_fragment(-2));
    view_index = view_name - lnav_view_strings;

    if (view_index < LNV__MAX && !regex.empty()) {
        lnav_data.ld_views[view_index].execute_search(regex);
        lnav_data.ld_views[view_index].set_follow_search_for(-1);
    }

    return 1;
}

static int read_top_line(yajlpp_parse_context *ypc, long long value)
{
    const char **         view_name;
    int view_index;

    view_name = find(lnav_view_strings,
                     lnav_view_strings + LNV__MAX,
                     ypc->get_path_fragment(-2));
    view_index = view_name - lnav_view_strings;
    if (view_index < LNV__MAX) {
        textview_curses &tc = lnav_data.ld_views[view_index];

        if (value != -1 && value < tc.get_inner_height()) {
            tc.set_top(vis_line_t(value));
        }
    }

    return 1;
}

static int read_word_wrap(yajlpp_parse_context *ypc, int value)
{
    const char **         view_name;
    int view_index;

    view_name = find(lnav_view_strings,
                     lnav_view_strings + LNV__MAX,
                     ypc->get_path_fragment(-2));
    view_index = view_name - lnav_view_strings;
    if (view_index == LNV_HELP) {

    }
    else if (view_index < LNV__MAX) {
        textview_curses &tc = lnav_data.ld_views[view_index];

        tc.set_word_wrap(value);
    }

    return 1;
}

static int read_commands(yajlpp_parse_context *ypc, const unsigned char *str, size_t len)
{
    std::string cmdline = std::string((const char *)str, len);
    const char **         view_name;
    int view_index;

    view_name = find(lnav_view_strings,
            lnav_view_strings + LNV__MAX,
            ypc->get_path_fragment(-3));
    view_index = view_name - lnav_view_strings;
    bool active = ensure_view(&lnav_data.ld_views[view_index]);
    execute_command(lnav_data.ld_exec_context, cmdline);
    if (!active) {
        lnav_data.ld_view_stack.vs_views.pop_back();
    }

    return 1;
}

static struct json_path_container view_def_handlers = {
    json_path_handler("top_line",  read_top_line),
    json_path_handler("search",    read_last_search),
    json_path_handler("word_wrap", read_word_wrap),
    json_path_handler("commands#", read_commands)
};

static struct json_path_container view_handlers = {
    json_path_handler(pcrepp("([^/]+)"))
        .with_children(view_def_handlers)
};

static struct json_path_container view_info_handlers = {
    json_path_handler("save-time",               read_save_time),
    json_path_handler("time-offset",             read_time_offset),
    json_path_handler("files#",                  read_files),
    json_path_handler("views")
        .with_children(view_handlers)
};

void load_session()
{
    load_time_bookmarks();
    scan_sessions() | [](const auto pair) {
        yajl_handle handle;
        auto_fd fd;

        lnav_data.ld_session_load_time = pair.first.second;
        lnav_data.ld_session_save_time = pair.first.second;
        const string &view_info_name = pair.second;

        yajlpp_parse_context ypc(view_info_name, &view_info_handlers);
        handle = yajl_alloc(&ypc.ypc_callbacks, nullptr, &ypc);

        load_time_bookmarks();

        if ((fd = open(view_info_name.c_str(), O_RDONLY)) < 0) {
            perror("cannot open session file");
        }
        else {
            unsigned char buffer[1024];
            ssize_t        rc;

            log_info("loading session file: %s", view_info_name.c_str());
            while ((rc = read(fd, buffer, sizeof(buffer))) > 0) {
                yajl_parse(handle, buffer, rc);
            }
            yajl_complete_parse(handle);
        }
        yajl_free(handle);
    };
}

static void yajl_writer(void *context, const char *str, size_t len)
{
    FILE *file = (FILE *)context;

    fwrite(str, len, 1, file);
}

static void save_user_bookmarks(
    sqlite3 *db, sqlite3_stmt *stmt, bookmark_vector<content_line_t> &user_marks)
{
    logfile_sub_source &lss = lnav_data.ld_log_source;
    std::map<content_line_t, bookmark_metadata> &bm_meta =
        lss.get_user_bookmark_metadata();
    bookmark_vector<content_line_t>::iterator iter;

    for (iter = user_marks.begin(); iter != user_marks.end(); ++iter) {
        std::map<content_line_t, bookmark_metadata>::iterator meta_iter;
        content_line_t cl = *iter;

        meta_iter = bm_meta.find(cl);

        if (!bind_line(db, stmt, cl, lnav_data.ld_session_time)) {
            continue;
        }

        if (meta_iter == bm_meta.end()) {
            if (sqlite3_bind_text(stmt, 5, "", 0, SQLITE_TRANSIENT) != SQLITE_OK) {
                log_error("could not bind log hash -- %s\n",
                        sqlite3_errmsg(db));
                return;
            }
        }
        else {
            if (meta_iter->second.empty()) {
                continue;
            }

            if (sqlite3_bind_text(stmt, 5,
                                  meta_iter->second.bm_name.c_str(),
                                  meta_iter->second.bm_name.length(),
                                  SQLITE_TRANSIENT) != SQLITE_OK) {
                log_error("could not bind part name -- %s\n",
                        sqlite3_errmsg(db));
                return;
            }

            bookmark_metadata &line_meta = meta_iter->second;
            if (sqlite3_bind_text(stmt, 6,
                                  meta_iter->second.bm_comment.c_str(),
                                  meta_iter->second.bm_comment.length(),
                                  SQLITE_TRANSIENT) != SQLITE_OK) {
                log_error("could not bind comment -- %s\n",
                          sqlite3_errmsg(db));
                return;
            }

            string tags;

            if (!line_meta.bm_tags.empty()) {
                yajlpp_gen gen;

                yajl_gen_config(gen, yajl_gen_beautify, false);

                {
                    yajlpp_array arr(gen);

                    for (const auto &str : line_meta.bm_tags) {
                        arr.gen(str);
                    }
                }

                tags = gen.to_string_fragment().to_string();
            }

            if (sqlite3_bind_text(stmt, 7,
                                  tags.c_str(),
                                  tags.length(),
                                  SQLITE_TRANSIENT) != SQLITE_OK) {
                log_error("could not bind tags -- %s\n",
                          sqlite3_errmsg(db));
                return;
            }
        }

        if (sqlite3_step(stmt) != SQLITE_DONE) {
            log_error(
                    "could not execute bookmark insert statement -- %s\n",
                    sqlite3_errmsg(db));
            return;
        }

        marked_session_lines.push_back(cl);

        sqlite3_reset(stmt);
    }

}

static void save_time_bookmarks()
{
    auto_mem<sqlite3, sqlite_close_wrapper> db;
    auto db_path = dotlnav_path() / LOG_METADATA_NAME;
    auto_mem<char, sqlite3_free> errmsg;
    auto_mem<sqlite3_stmt> stmt(sqlite3_finalize);

    if (sqlite3_open(db_path.str().c_str(), db.out()) != SQLITE_OK) {
        log_error("unable to open bookmark DB -- %s\n", db_path.str().c_str());
        return;
    }

    if (sqlite3_exec(db.in(), BOOKMARK_TABLE_DEF, NULL, NULL, errmsg.out()) != SQLITE_OK) {
        log_error("unable to make bookmark table -- %s\n", errmsg.in());
        return;
    }

    if (sqlite3_exec(db.in(), "BEGIN TRANSACTION", NULL, NULL, errmsg.out()) != SQLITE_OK) {
        log_error("unable to begin transaction -- %s\n", errmsg.in());
        return;
    }

    logfile_sub_source &lss = lnav_data.ld_log_source;
    bookmarks<content_line_t>::type &bm = lss.get_user_bookmarks();

    if (sqlite3_prepare_v2(db.in(),
                           "DELETE FROM bookmarks WHERE "
                           " log_time = ? and log_format = ? and log_hash = ? "
                           " and session_time = ?",
                           -1,
                           stmt.out(),
                           nullptr) != SQLITE_OK) {
        log_error(
                "could not prepare bookmark delete statement -- %s\n",
                sqlite3_errmsg(db));
        return;
    }

    for (auto &marked_session_line : marked_session_lines) {
        if (!bind_line(
            db.in(), stmt.in(), marked_session_line, lnav_data.ld_session_time)) {
            continue;
        }

        if (sqlite3_step(stmt.in()) != SQLITE_DONE) {
            log_error(
                    "could not execute bookmark insert statement -- %s\n",
                    sqlite3_errmsg(db));
            return;
        }

        sqlite3_reset(stmt.in());
    }

    marked_session_lines.clear();

    if (sqlite3_prepare_v2(db.in(),
                           "REPLACE INTO bookmarks"
                           " (log_time, log_format, log_hash, session_time, part_name, comment, tags)"
                           " VALUES (?, ?, ?, ?, ?, ?, ?)",
                           -1,
                           stmt.out(),
                           nullptr) != SQLITE_OK) {
        log_error(
                "could not prepare bookmark replace statement -- %s\n",
                sqlite3_errmsg(db));
        return;
    }

    {
        logfile_sub_source::iterator file_iter;

        for (file_iter = lnav_data.ld_log_source.begin();
             file_iter != lnav_data.ld_log_source.end();
             ++file_iter) {
            shared_ptr<logfile> lf = (*file_iter)->get_file();
            content_line_t base_content_line;

            if (lf == nullptr)
                continue;

            base_content_line = lss.get_file_base_content_line(file_iter);
            base_content_line = content_line_t(
                base_content_line + lf->size() - 1);

            if (!bind_line(db.in(), stmt.in(), base_content_line,
                lnav_data.ld_session_time)) {
                continue;
            }

            if (sqlite3_bind_null(stmt.in(), 5) != SQLITE_OK) {
                log_error("could not bind log hash -- %s\n",
                        sqlite3_errmsg(db.in()));
                return;
            }

            if (sqlite3_step(stmt.in()) != SQLITE_DONE) {
                log_error(
                        "could not execute bookmark insert statement -- %s\n",
                        sqlite3_errmsg(db));
                return;
            }

            sqlite3_reset(stmt.in());
        }
    }

    save_user_bookmarks(db.in(), stmt.in(), bm[&textview_curses::BM_USER]);
    save_user_bookmarks(db.in(), stmt.in(), bm[&textview_curses::BM_META]);

    if (sqlite3_prepare_v2(db.in(),
                           "DELETE FROM time_offset WHERE "
                           " log_time = ? and log_format = ? and log_hash = ? "
                           " and session_time = ?",
                           -1,
                           stmt.out(),
                           NULL) != SQLITE_OK) {
        log_error(
                "could not prepare time_offset delete statement -- %s\n",
                sqlite3_errmsg(db));
        return;
    }

    for (auto &offset_session_line : offset_session_lines) {
        if (!bind_line(
            db.in(), stmt.in(), offset_session_line, lnav_data.ld_session_time)) {
            continue;
        }

        if (sqlite3_step(stmt.in()) != SQLITE_DONE) {
            log_error(
                    "could not execute bookmark insert statement -- %s\n",
                    sqlite3_errmsg(db));
            return;
        }

        sqlite3_reset(stmt.in());
    }

    offset_session_lines.clear();

    if (sqlite3_prepare_v2(db.in(),
                           "REPLACE INTO time_offset"
                           " (log_time, log_format, log_hash, session_time, offset_sec, offset_usec)"
                           " VALUES (?, ?, ?, ?, ?, ?)",
                           -1,
                           stmt.out(),
                           NULL) != SQLITE_OK) {
        log_error(
                "could not prepare time_offset replace statement -- %s\n",
                sqlite3_errmsg(db));
        return;
    }

    {
        logfile_sub_source::iterator file_iter;

        for (file_iter = lnav_data.ld_log_source.begin();
             file_iter != lnav_data.ld_log_source.end();
             ++file_iter) {
            shared_ptr<logfile> lf = (*file_iter)->get_file();
            content_line_t base_content_line;

            if (lf == nullptr) {
                continue;
            }

            base_content_line = lss.get_file_base_content_line(file_iter);

            if (!bind_line(db.in(), stmt.in(), base_content_line,
                lnav_data.ld_session_time)) {
                continue;
            }

            if (sqlite3_bind_null(stmt.in(), 5) != SQLITE_OK) {
                log_error("could not bind log hash -- %s\n",
                        sqlite3_errmsg(db.in()));
                return;
            }

            if (sqlite3_bind_null(stmt.in(), 6) != SQLITE_OK) {
                log_error("could not bind log hash -- %s\n",
                        sqlite3_errmsg(db.in()));
                return;
            }

            if (sqlite3_step(stmt.in()) != SQLITE_DONE) {
                log_error(
                        "could not execute bookmark insert statement -- %s\n",
                        sqlite3_errmsg(db));
                return;
            }

            sqlite3_reset(stmt.in());
        }
    }

    for (auto &ls : lss) {
        logfile::iterator line_iter;

        if (ls->get_file() == NULL)
            continue;

        shared_ptr<logfile> lf = ls->get_file();

        if (!lf->is_time_adjusted()) {
            continue;
        }

        line_iter = lf->begin() + lf->get_time_offset_line();
        struct timeval offset = lf->get_time_offset();

        auto read_result = lf->read_line(line_iter);

        if (read_result.isErr()) {
            return;
        }

        bind_values(stmt.in(),
                    lf->original_line_time(line_iter),
                    lf->get_format()->get_name(),
                    read_result.map([](auto sbr) {
                        return hash_bytes(sbr.get_data(), sbr.length(), nullptr);
                    }).unwrap(),
                    lnav_data.ld_session_time,
                    offset.tv_sec,
                    offset.tv_usec);

        if (sqlite3_step(stmt.in()) != SQLITE_DONE) {
            log_error(
                    "could not execute bookmark insert statement -- %s\n",
                    sqlite3_errmsg(db));
            return;
        }

        sqlite3_reset(stmt.in());
    }

    if (sqlite3_exec(db.in(), "COMMIT", NULL, NULL, errmsg.out()) != SQLITE_OK) {
        log_error("unable to begin transaction -- %s\n", errmsg.in());
        return;
    }

    if (sqlite3_exec(db.in(), BOOKMARK_LRU_STMT, NULL, NULL, errmsg.out()) != SQLITE_OK) {
        log_error("unable to delete old bookmarks -- %s\n", errmsg.in());
        return;
    }
}

static void save_session_with_id(const std::string session_id)
{
    auto_mem<FILE> file(fclose);
    yajl_gen handle = nullptr;

    /* TODO: save the last search query */

    log_info("saving session with id: %s", session_id.c_str());

    char view_base_name[256];
    snprintf(view_base_name, sizeof(view_base_name),
             "view-info-%s.ts%ld.ppid%d.json",
             session_id.c_str(),
             lnav_data.ld_session_time,
             getppid());

    auto view_file_name = dotlnav_path() / view_base_name;
    auto view_file_tmp_name = view_file_name + ".tmp";

    if ((file = fopen(view_file_tmp_name.str().c_str(), "w")) == nullptr) {
        perror("Unable to open session file");
    }
    else if (nullptr == (handle = yajl_gen_alloc(nullptr))) {
        perror("Unable to create yajl_gen object");
    }
    else {
        yajl_gen_config(handle,
                        yajl_gen_print_callback, yajl_writer, file.in());

        {
            yajlpp_map root_map(handle);

            root_map.gen("save-time");
            root_map.gen((long long) time(NULL));

            root_map.gen("time-offset");
            root_map.gen(lnav_data.ld_log_source.is_time_offset_enabled());

            root_map.gen("files");

            {
                yajlpp_array file_list(handle);

                for (auto &ld_file_name : lnav_data.ld_file_names) {
                    file_list.gen(ld_file_name.first);
                }
            }

            root_map.gen("views");

            {
                yajlpp_map top_view_map(handle);

                for (int lpc = 0; lpc < LNV__MAX; lpc++) {
                    textview_curses &tc = lnav_data.ld_views[lpc];
                    unsigned long    width;
                    vis_line_t       height;

                    top_view_map.gen(lnav_view_strings[lpc]);

                    yajlpp_map view_map(handle);

                    view_map.gen("top_line");

                    tc.get_dimensions(height, width);
                    if (tc.get_top() >= tc.get_top_for_last_row()) {
                        view_map.gen(-1LL);
                    }
                    else{
                        view_map.gen((long long)tc.get_top());
                    }

                    view_map.gen("search");
                    view_map.gen(lnav_data.ld_views[lpc].get_last_search());

                    view_map.gen("word_wrap");
                    view_map.gen(tc.get_word_wrap());

                    text_sub_source *tss = tc.get_sub_source();
                    if (tss == nullptr) {
                        continue;
                    }

                    filter_stack &fs = tss->get_filters();

                    view_map.gen("commands");
                    yajlpp_array cmd_array(handle);

                    for (const auto filter : fs) {
                        string cmd = filter->to_command();

                        if (cmd.empty()) {
                            continue;
                        }

                        cmd_array.gen(cmd);

                        if (!filter->is_enabled()) {
                            cmd_array.gen("disable-filter " + filter->get_id());
                        }
                    }

                    textview_curses::highlight_map_t &hmap =
                        lnav_data.ld_views[lpc].get_highlights();
                    textview_curses::highlight_map_t::iterator hl_iter;

                    for (hl_iter = hmap.begin();
                         hl_iter != hmap.end();
                         ++hl_iter) {
                        if (hl_iter->first.first != highlight_source_t::INTERACTIVE) {
                            continue;
                        }
                        cmd_array.gen("highlight " + hl_iter->first.second);
                    }

                    if (lpc == LNV_LOG) {
                        for (auto format : log_format::get_root_formats()) {
                            auto *elf = dynamic_cast<external_log_format *>(format);

                            if (elf == nullptr) {
                                continue;
                            }

                            for (auto vd : elf->elf_value_defs) {
                                if (!vd.second->vd_user_hidden) {
                                    continue;
                                }

                                cmd_array.gen("hide-fields "
                                              + elf->get_name().to_string()
                                              + "."
                                              + vd.first.to_string());
                            }
                        }

                        logfile_sub_source &lss = lnav_data.ld_log_source;

                        struct timeval min_time, max_time;
                        bool have_min_time = lss.get_min_log_time(min_time);
                        bool have_max_time = lss.get_max_log_time(max_time);
                        char min_time_str[32], max_time_str[32];

                        sql_strftime(min_time_str, sizeof(min_time_str), min_time);
                        if (have_min_time) {
                            cmd_array.gen("hide-lines-before "
                                          + string(min_time_str));
                        }
                        if (have_max_time) {
                            sql_strftime(max_time_str, sizeof(max_time_str), max_time);
                            cmd_array.gen("hide-lines-after "
                                          + string(max_time_str));
                        }
                    }
                }
            }
        }

        yajl_gen_clear(handle);
        yajl_gen_free(handle);

        fclose(file.release());

        log_perror(rename(view_file_tmp_name.str().c_str(),
            view_file_name.str().c_str()));

        log_info("Saved session: %s", view_file_name.str().c_str());
    }
}

void save_session()
{
    save_time_bookmarks();

    const auto opt_session_id = compute_session_id();
    opt_session_id | [](auto &session_id) {
        save_session_with_id(session_id);
    };
    for (const auto pair : lnav_data.ld_session_id) {
        if (opt_session_id && pair.first == opt_session_id.value()) {
            continue;
        }
        save_session_with_id(pair.first);
    }
}

void reset_session()
{
    log_info("reset session: time=%d", lnav_data.ld_session_time);

    save_session();

    lnav_data.ld_session_time = time(nullptr);

    for (auto &tc : lnav_data.ld_views) {
        auto &hmap = tc.get_highlights();
        auto hl_iter = hmap.begin();

        while (hl_iter != hmap.end()) {
            if (hl_iter->first.first != highlight_source_t::INTERACTIVE) {
                ++hl_iter;
            } else {
                hmap.erase(hl_iter++);
            }
        }
    }

    for (auto ld : lnav_data.ld_log_source) {
        shared_ptr<logfile> lf = ld->get_file();

        lf->clear_time_offset();
    }

    lnav_data.ld_log_source.set_marked_only(false);
    lnav_data.ld_log_source.clear_min_max_log_times();
    lnav_data.ld_log_source.set_min_log_level(LEVEL_UNKNOWN);

    lnav_data.ld_log_source.get_user_bookmark_metadata().clear();

    for (auto &tc : lnav_data.ld_views) {
        text_sub_source *tss = tc.get_sub_source();

        if (tss == nullptr) {
            continue;
        }
        tss->get_filters().clear_filters();
        tss->text_filters_changed();
        tss->text_clear_marks(&textview_curses::BM_USER);
        tc.get_bookmarks()[&textview_curses::BM_USER].clear();
        tss->text_clear_marks(&textview_curses::BM_META);
        tc.get_bookmarks()[&textview_curses::BM_META].clear();
        tc.reload_data();
    }

    lnav_data.ld_filter_view.reload_data();
    for (auto format : log_format::get_root_formats()) {
        auto *elf = dynamic_cast<external_log_format *>(format);

        if (elf == nullptr) {
            continue;
        }

        for (const auto &vd : elf->elf_value_defs) {
            vd.second->vd_user_hidden = false;
        }
    }
}
