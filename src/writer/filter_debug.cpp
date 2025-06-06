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
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <ostream>

#include <dwarfs/writer/entry_interface.h>
#include <dwarfs/writer/filter_debug.h>

namespace dwarfs::writer {

void debug_filter_output(std::ostream& os, bool exclude,
                         entry_interface const& ei, debug_filter_mode mode) {
  if (exclude ? mode == debug_filter_mode::INCLUDED or
                    mode == debug_filter_mode::INCLUDED_FILES
              : mode == debug_filter_mode::EXCLUDED or
                    mode == debug_filter_mode::EXCLUDED_FILES) {
    return;
  }

  bool const files_only = mode == debug_filter_mode::FILES or
                          mode == debug_filter_mode::INCLUDED_FILES or
                          mode == debug_filter_mode::EXCLUDED_FILES;

  if (files_only and ei.is_directory()) {
    return;
  }

  char const* prefix = "";

  if (mode == debug_filter_mode::FILES or mode == debug_filter_mode::ALL) {
    prefix = exclude ? "- " : "+ ";
  }

  os << prefix << ei.unix_dpath() << "\n";
}

} // namespace dwarfs::writer
