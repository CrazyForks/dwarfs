/* vim:set ts=2 sw=2 sts=2 et: */
/**
 * \author     Marcus Holland-Moritz (github@mhxnet.de)
 * \copyright  Copyright (c) Marcus Holland-Moritz
 *
 * This file is part of dwarfs.
 *
 * dwarfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * dwarfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with dwarfs.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <fmt/format.h>

template <typename T, typename U>
struct fmt::formatter<std::pair<T, U>> {
 public:
  constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
  template <typename Context>
  constexpr auto format(std::pair<T, U> const& foo, Context& ctx) const {
    return fmt::format_to(ctx.out(), "({}, {})", foo.first, foo.second);
  }
};