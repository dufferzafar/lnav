/**
 * Copyright (c) 2022, Timothy Stack
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
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <algorithm>

#include "lnav.console.hh"

#include "config.h"
#include "fmt/color.h"
#include "itertools.hh"
#include "log_level_enum.hh"
#include "view_curses.hh"

using namespace lnav::roles::literals;

namespace lnav {
namespace console {

user_message
user_message::raw(const attr_line_t& al)
{
    user_message retval;

    retval.um_level = level::raw;
    retval.um_message.append(al);
    return retval;
}

user_message
user_message::error(const attr_line_t& al)
{
    user_message retval;

    retval.um_level = level::error;
    retval.um_message.append(al);
    return retval;
}

user_message
user_message::info(const attr_line_t& al)
{
    user_message retval;

    retval.um_level = level::info;
    retval.um_message.append(al);
    return retval;
}

user_message
user_message::ok(const attr_line_t& al)
{
    user_message retval;

    retval.um_level = level::ok;
    retval.um_message.append(al);
    return retval;
}

user_message
user_message::warning(const attr_line_t& al)
{
    user_message retval;

    retval.um_level = level::warning;
    retval.um_message.append(al);
    return retval;
}

attr_line_t
user_message::to_attr_line(std::set<render_flags> flags) const
{
    auto indent = 1;
    attr_line_t retval;

    if (this->um_level == level::warning) {
        indent = 3;
    }

    if (flags.count(render_flags::prefix)) {
        switch (this->um_level) {
            case level::raw:
                break;
            case level::ok:
                retval.append(lnav::roles::ok("\u2714 "));
                break;
            case level::info:
                retval.append(lnav::roles::status("\u24d8 info")).append(": ");
                break;
            case level::warning:
                retval.append(lnav::roles::warning("\u26a0 warning"))
                    .append(": ");
                break;
            case level::error:
                retval.append(lnav::roles::error("\u2718 error")).append(": ");
                break;
        }
    }

    retval.append(this->um_message).append("\n");
    if (!this->um_reason.empty()) {
        bool first_line = true;
        for (const auto& line : this->um_reason.split_lines()) {
            auto role = this->um_level == level::error ? role_t::VCR_ERROR
                                                       : role_t::VCR_WARNING;
            attr_line_t prefix;

            if (first_line) {
                prefix.append(indent, ' ')
                    .append("reason", VC_ROLE.value(role))
                    .append(": ");
                first_line = false;
            } else {
                prefix.append(" |      ", VC_ROLE.value(role))
                    .append(indent, ' ');
            }
            retval.append(prefix).append(line).append("\n");
        }
    }
    if (!this->um_snippets.empty()) {
        for (const auto& snip : this->um_snippets) {
            attr_line_t header;

            header.append(" --> "_comment)
                .append(lnav::roles::file(snip.s_location.sl_source.get()));
            if (snip.s_location.sl_line_number > 0) {
                header.append(":").append(FMT_STRING("{}"),
                                          snip.s_location.sl_line_number);
            }
            retval.append(header).append("\n");
            if (!snip.s_content.blank()) {
                for (const auto& line : snip.s_content.split_lines()) {
                    retval.append(" | "_comment).append(line).append("\n");
                }
            }
        }
    }
    if (!this->um_notes.empty()) {
        for (const auto& note : this->um_notes) {
            bool first_line = true;
            for (const auto& line : note.split_lines()) {
                attr_line_t prefix;

                if (first_line) {
                    prefix.append(" ="_comment)
                        .append(indent, ' ')
                        .append("note"_comment)
                        .append(": ");
                    first_line = false;
                } else {
                    prefix.append("        ").append(indent, ' ');
                }

                retval.append(prefix).append(line).append("\n");
            }
        }
    }
    if (!this->um_help.empty()) {
        bool first_line = true;
        for (const auto& line : this->um_help.split_lines()) {
            attr_line_t prefix;

            if (first_line) {
                prefix.append(" ="_comment)
                    .append(indent, ' ')
                    .append("help"_comment)
                    .append(": ");
                first_line = false;
            } else {
                prefix.append("         ");
            }

            retval.append(prefix).append(line).append("\n");
        }
    }

    return retval;
}

fmt::terminal_color
curses_color_to_terminal_color(int curses_color)
{
    switch (curses_color) {
        case COLOR_BLACK:
            return fmt::terminal_color::black;
        case COLOR_CYAN:
            return fmt::terminal_color::cyan;
        case COLOR_WHITE:
            return fmt::terminal_color::white;
        case COLOR_MAGENTA:
            return fmt::terminal_color::magenta;
        case COLOR_BLUE:
            return fmt::terminal_color::blue;
        case COLOR_YELLOW:
            return fmt::terminal_color::yellow;
        case COLOR_GREEN:
            return fmt::terminal_color::green;
        case COLOR_RED:
            return fmt::terminal_color::red;
    }

    ensure(false);
}

void
println(FILE* file, const attr_line_t& al)
{
    const auto& str = al.get_string();

    if (getenv("NO_COLOR") != nullptr
        || (!isatty(fileno(file)) && getenv("YES_COLOR") == nullptr))
    {
        fmt::print(file, "{}\n", str);
        return;
    }

    std::set<int> points = {0, (int) al.length()};

    for (const auto& attr : al.get_attrs()) {
        if (!attr.sa_range.is_valid()) {
            continue;
        }
        points.insert(attr.sa_range.lr_start);
        if (attr.sa_range.lr_end > 0) {
            points.insert(attr.sa_range.lr_end);
        }
    }

    nonstd::optional<int> last_point;
    for (const auto& point : points) {
        if (last_point) {
            auto line_style = fmt::text_style{};
            auto fg_style = fmt::text_style{};
            auto start = last_point.value();

            for (const auto& attr : al.get_attrs()) {
                if (!attr.sa_range.contains(start)
                    && !attr.sa_range.contains(point - 1)) {
                    continue;
                }

                if (attr.sa_type == &VC_BACKGROUND) {
                    auto saw = string_attr_wrapper<int64_t>(&attr);
                    auto color = saw.get();

                    if (color >= 0) {
                        line_style
                            |= fmt::bg(curses_color_to_terminal_color(color));
                    }
                } else if (attr.sa_type == &VC_FOREGROUND) {
                    auto saw = string_attr_wrapper<int64_t>(&attr);
                    auto color = saw.get();

                    if (color >= 0) {
                        fg_style
                            = fmt::fg(curses_color_to_terminal_color(color));
                    }
                } else if (attr.sa_type == &VC_ROLE) {
                    auto saw = string_attr_wrapper<role_t>(&attr);
                    auto role = saw.get();

                    switch (role) {
                        case role_t::VCR_ERROR:
                            line_style |= fmt::fg(fmt::terminal_color::red);
                            break;
                        case role_t::VCR_WARNING:
                            line_style |= fmt::fg(fmt::terminal_color::yellow);
                            break;
                        case role_t::VCR_COMMENT:
                            line_style |= fmt::fg(fmt::terminal_color::cyan);
                            break;
                        case role_t::VCR_OK:
                            line_style |= fmt::emphasis::bold
                                | fmt::fg(fmt::terminal_color::green);
                            break;
                        case role_t::VCR_STATUS:
                            line_style |= fmt::emphasis::bold
                                | fmt::fg(fmt::terminal_color::magenta);
                            break;
                        case role_t::VCR_KEYWORD:
                            line_style |= fmt::emphasis::bold
                                | fmt::fg(fmt::terminal_color::blue);
                            break;
                        case role_t::VCR_VARIABLE:
                            line_style |= fmt::emphasis::underline;
                            break;
                        case role_t::VCR_SYMBOL:
                        case role_t::VCR_NUMBER:
                        case role_t::VCR_FILE:
                            line_style |= fmt::emphasis::bold;
                            break;
                        case role_t::VCR_H1:
                            line_style |= fmt::emphasis::bold
                                | fmt::fg(fmt::terminal_color::magenta);
                            break;
                        case role_t::VCR_H2:
                            line_style |= fmt::emphasis::bold;
                            break;
                        case role_t::VCR_H3:
                        case role_t::VCR_H4:
                        case role_t::VCR_H5:
                        case role_t::VCR_H6:
                            line_style |= fmt::emphasis::underline;
                            break;
                        case role_t::VCR_LIST_GLYPH:
                            line_style |= fmt::fg(fmt::terminal_color::yellow);
                            break;
                        default:
                            break;
                    }
                }
            }

            if (!line_style.has_foreground() && fg_style.has_foreground()) {
                line_style |= fg_style;
            }

            fmt::print(file,
                       line_style,
                       FMT_STRING("{}"),
                       str.substr(start, point - start));
        }
        last_point = point;
    }
    fmt::print(file, "\n");
}

void
print(FILE* file, const user_message& um)
{
    println(file, um.to_attr_line().rtrim());
}

}  // namespace console
}  // namespace lnav
