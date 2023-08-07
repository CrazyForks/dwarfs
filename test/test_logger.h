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

#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <folly/Conv.h>

#include "dwarfs/logger.h"

namespace dwarfs::test {

class test_logger : public ::dwarfs::logger {
 public:
  struct log_entry {
    log_entry(level_type level, std::string const& output, char const* file,
              int line)
        : level{level}
        , output{output}
        , file{file}
        , line{line} {}

    level_type level;
    std::string output;
    char const* file;
    int line;
  };

  test_logger(level_type threshold = TRACE)
      : threshold_{threshold}
      , output_{debug_output_enabled()} {
    if (output_ || threshold > level_type::INFO) {
      set_policy<debug_logger_policy>();
    } else {
      set_policy<prod_logger_policy>();
    }
  }

  void write(level_type level, std::string const& output, char const* file,
             int line) override {
    if (output_) {
      std::lock_guard lock(mx_);
      std::cerr << level_char(level) << " [" << file << ":" << line << "] "
                << output << "\n";
    }

    if (level <= threshold_) {
      std::lock_guard lock(mx_);
      log_.emplace_back(level, output, file, line);
    }
  }

  std::vector<log_entry> const& get_log() const { return log_; }

  bool empty() const { return log_.empty(); }

  void clear() { log_.clear(); }

 private:
  static bool debug_output_enabled() {
    if (auto var = std::getenv("DWARFS_TEST_LOGGER_OUTPUT")) {
      if (auto val = folly::tryTo<bool>(var); val && *val) {
        return true;
      }
    }
    return false;
  }

  std::mutex mx_;
  std::vector<log_entry> log_;
  level_type const threshold_;
  bool const output_;
};

} // namespace dwarfs::test