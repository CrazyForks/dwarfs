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

#include <algorithm>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string_view>
#include <vector>

#include <boost/program_options.hpp>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include <folly/String.h>
#include <folly/gen/String.h>
#include <folly/json.h>
#include <folly/portability/Unistd.h>
#include <folly/system/HardwareConcurrency.h>

#include "dwarfs/checksum.h"
#include "dwarfs/error.h"
#include "dwarfs/file_access.h"
#include "dwarfs/filesystem_v2.h"
#include "dwarfs/iolayer.h"
#include "dwarfs/library_dependencies.h"
#include "dwarfs/logger.h"
#include "dwarfs/mmap.h"
#include "dwarfs/options.h"
#include "dwarfs/os_access.h"
#include "dwarfs/program_options_helpers.h"
#include "dwarfs/tool.h"
#include "dwarfs/util.h"
#include "dwarfs/worker_group.h"
#include "dwarfs_tool_main.h"

namespace dwarfs {

namespace po = boost::program_options;

namespace {

void do_list_files(filesystem_v2& fs, iolayer const& iol, bool verbose) {
  auto max_width = [](auto const& vec) {
    auto max = std::max_element(vec.begin(), vec.end());
    return std::to_string(*max).size();
  };

  auto const uid_width = max_width(fs.get_all_uids());
  auto const gid_width = max_width(fs.get_all_gids());

  file_stat::off_type max_inode_size{0};
  fs.walk([&](auto const& de) {
    file_stat st;
    fs.getattr(de.inode(), &st);
    max_inode_size = std::max(max_inode_size, st.size);
  });

  auto const inode_size_width = fmt::format("{:L}", max_inode_size).size();

  fs.walk([&](auto const& de) {
    auto iv = de.inode();
    file_stat st;
    fs.getattr(iv, &st);
    auto name = de.unix_path();
    utf8_sanitize(name);

    if (verbose) {
      if (iv.is_symlink()) {
        auto target = fs.readlink(iv).value();
        utf8_sanitize(target);
        name += " -> " + target;
      }

      iol.out << fmt::format(
          "{3} {4:{0}}/{5:{1}} {6:{2}L} {7:%Y-%m-%d %H:%M} {8}\n", uid_width,
          gid_width, inode_size_width, iv.mode_string(), iv.getuid(),
          iv.getgid(), st.size, fmt::localtime(st.mtime), name);
    } else if (!name.empty()) {
      iol.out << name << "\n";
    }
  });
}

void do_checksum(logger& lgr, filesystem_v2& fs, iolayer const& iol,
                 std::string const& algo, size_t num_workers) {
  LOG_PROXY(debug_logger_policy, lgr);

  worker_group wg{lgr, *iol.os, "checksum", num_workers};
  std::mutex mx;

  fs.walk_data_order([&](auto const& de) {
    auto iv = de.inode();

    if (iv.is_regular_file()) {
      file_stat st;

      if (fs.getattr(de.inode(), &st) != 0) {
        LOG_ERROR << "failed to get attributes for inode " << iv.inode_num();
        return;
      }

      auto ranges = fs.readv(iv.inode_num(), st.size);

      if (!ranges) {
        LOG_ERROR << "failed to read inode " << iv.inode_num() << ": "
                  << std::strerror(-ranges.error());
        return;
      }

      wg.add_job([&, de, iv, ranges = std::move(ranges)]() mutable {
        checksum cs(algo);

        for (auto& fut : ranges.value()) {
          try {
            auto range = fut.get();
            cs.update(range.data(), range.size());
          } catch (std::exception const& e) {
            LOG_ERROR << "error reading data from inode " << iv.inode_num()
                      << ": " << e.what();
            return;
          }
        }

        auto output = fmt::format("{}  {}\n", cs.hexdigest(), de.unix_path());

        {
          std::lock_guard lock(mx);
          iol.out << output;
        }
      });
    }
  });

  wg.wait();
}

} // namespace

int dwarfsck_main(int argc, sys_char** argv, iolayer const& iol) {
  using namespace folly::gen;

  const size_t num_cpu = std::max(folly::hardware_concurrency(), 1u);

  auto algo_list = checksum::available_algorithms();
  auto checksum_desc = "print checksums for all files (" +
                       (from(algo_list) | unsplit(", ")) + ")";

  sys_string input, export_metadata;
  std::string image_offset, checksum_algo;
  logger_options logopts;
  size_t num_workers;
  int detail;
  bool quiet{false};
  bool verbose{false};
  bool output_json{false};
  bool check_integrity{false};
  bool no_check{false};
  bool print_header{false};
  bool list_files{false};

  // clang-format off
  po::options_description opts("Command line options");
  opts.add_options()
    ("input,i",
        po_sys_value<sys_string>(&input),
        "input filesystem")
    ("detail,d",
        po::value<int>(&detail)->default_value(2),
        "detail level")
    ("quiet,q",
        po::value<bool>(&quiet)->zero_tokens(),
        "don't print anything unless an error occurs")
    ("verbose,v",
        po::value<bool>(&verbose)->zero_tokens(),
        "produce verbose output")
    ("image-offset,O",
        po::value<std::string>(&image_offset)->default_value("auto"),
        "filesystem image offset in bytes")
    ("print-header,H",
        po::value<bool>(&print_header)->zero_tokens(),
        "print filesystem header to stdout and exit")
    ("list,l",
        po::value<bool>(&list_files)->zero_tokens(),
        "list all files and exit")
    ("checksum",
        po::value<std::string>(&checksum_algo),
        checksum_desc.c_str())
    ("num-workers,n",
        po::value<size_t>(&num_workers)->default_value(num_cpu),
        "number of reader worker threads")
    ("check-integrity",
        po::value<bool>(&check_integrity)->zero_tokens(),
        "check integrity of each block")
    ("no-check",
        po::value<bool>(&no_check)->zero_tokens(),
        "don't even verify block checksums")
    ("json,j",
        po::value<bool>(&output_json)->zero_tokens(),
        "print information in JSON format")
    ("export-metadata",
        po_sys_value<sys_string>(&export_metadata),
        "export raw metadata as JSON to file")
    ;
  // clang-format on

  add_common_options(opts, logopts);

  po::positional_options_description pos;
  pos.add("input", -1);

  po::variables_map vm;

  try {
    po::store(po::basic_command_line_parser<sys_char>(argc, argv)
                  .options(opts)
                  .positional(pos)
                  .run(),
              vm);
    po::notify(vm);
  } catch (po::error const& e) {
    iol.err << "error: " << e.what() << "\n";
    return 1;
  }

#ifdef DWARFS_BUILTIN_MANPAGE
  if (vm.count("man")) {
    show_manpage(manpage::get_dwarfsck_manpage(), iol);
    return 0;
  }
#endif

  auto constexpr usage = "Usage: dwarfsck [OPTIONS...]\n";

  if (vm.count("help") or !vm.count("input")) {
    iol.out << tool_header("dwarfsck")
            << library_dependencies::common_as_string() << "\n\n"
            << usage << "\n"
            << opts << "\n";
    return 0;
  }

  try {
    stream_logger lgr(iol.term, iol.err, logopts);
    LOG_PROXY(debug_logger_policy, lgr);

    if (no_check && check_integrity) {
      LOG_WARN << "--no-check and --check-integrity are mutually exclusive";
      return 1;
    }

    if (vm.count("checksum") && !checksum::is_available(checksum_algo)) {
      LOG_WARN << "checksum algorithm not available: " << checksum_algo;
      return 1;
    }

    if (print_header &&
        (output_json || !export_metadata.empty() || check_integrity ||
         list_files || !checksum_algo.empty())) {
      LOG_WARN << "--print-header is mutually exclusive with --json, "
                  "--export-metadata, --check-integrity, --list and --checksum";
      return 1;
    }

    filesystem_options fsopts;

    fsopts.metadata.enable_nlink = true;
    fsopts.metadata.check_consistency = check_integrity;
    fsopts.image_offset = parse_image_offset(image_offset);

    auto input_path = iol.os->canonical(input);

    std::shared_ptr<mmif> mm = iol.os->map_file(input_path);

    if (print_header) {
      if (auto hdr = filesystem_v2::header(mm, fsopts.image_offset)) {
#ifdef _WIN32
        if (&iol.out == &std::cout) {
          ::_setmode(::_fileno(stdout), _O_BINARY);
        }
#endif
        iol.out.write(reinterpret_cast<char const*>(hdr->data()), hdr->size());
        if (iol.out.bad() || iol.out.fail()) {
          LOG_ERROR << "error writing header";
          return 1;
        }
      } else {
        LOG_WARN << "filesystem does not contain a header";
        return 2;
      }
    } else {
      filesystem_v2 fs(lgr, *iol.os, mm, fsopts);

      if (!export_metadata.empty()) {
        std::error_code ec;
        auto of = iol.file->open_output(iol.os->canonical(export_metadata), ec);
        if (ec) {
          LOG_ERROR << "failed to open metadata output file: " << ec.message();
          return 1;
        }
        auto json = fs.serialize_metadata_as_json(false);
        of->os().write(json.data(), json.size());
        of->close(ec);
        if (ec) {
          LOG_ERROR << "failed to close metadata output file: " << ec.message();
          return 1;
        }
      } else {
        auto level = check_integrity ? filesystem_check_level::FULL
                                     : filesystem_check_level::CHECKSUM;
        auto errors = no_check ? 0 : fs.check(level, num_workers);

        if (!quiet && !list_files && checksum_algo.empty()) {
          if (output_json) {
            iol.out << folly::toPrettyJson(fs.info_as_dynamic(detail)) << "\n";
          } else {
            fs.dump(iol.out, detail);
          }
        }

        if (list_files) {
          do_list_files(fs, iol, verbose);
        }

        if (!checksum_algo.empty()) {
          do_checksum(lgr, fs, iol, checksum_algo, num_workers);
        }

        if (errors > 0) {
          return 1;
        }
      }
    }
  } catch (std::exception const& e) {
    iol.err << folly::exceptionStr(e) << "\n";
    return 1;
  }

  return 0;
}

int dwarfsck_main(int argc, sys_char** argv) {
  return dwarfsck_main(argc, argv, iolayer::system_default());
}

int dwarfsck_main(std::span<std::string> args, iolayer const& iol) {
  return call_sys_main_iolayer(args, iol, dwarfsck_main);
}

int dwarfsck_main(std::span<std::string_view> args, iolayer const& iol) {
  return call_sys_main_iolayer(args, iol, dwarfsck_main);
}

} // namespace dwarfs
