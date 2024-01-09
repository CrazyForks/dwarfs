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
#include <array>
#include <filesystem>
#include <iostream>
#include <random>
#include <set>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fmt/format.h>

#include <folly/String.h>
#include <folly/json.h>

#include "dwarfs/filesystem_v2.h"
#include "dwarfs/logger.h"
#include "dwarfs/util.h"
#include "dwarfs_tool_main.h"

#include "filter_test_data.h"
#include "loremipsum.h"
#include "mmap_mock.h"
#include "test_helpers.h"
#include "test_logger.h"

using namespace dwarfs;

namespace fs = std::filesystem;

namespace {

auto test_dir = fs::path(TEST_DATA_DIR).make_preferred();
auto audio_data_dir = test_dir / "pcmaudio";

enum class input_mode {
  from_file,
  from_stdin,
};

constexpr std::array<input_mode, 2> const input_modes = {
    input_mode::from_file, input_mode::from_stdin};

std::ostream& operator<<(std::ostream& os, input_mode m) {
  switch (m) {
  case input_mode::from_file:
    os << "from_file";
    break;
  case input_mode::from_stdin:
    os << "from_stdin";
    break;
  }
  return os;
}

struct locale_setup_helper {
  locale_setup_helper() { setup_default_locale(); }
};

void setup_locale() { static locale_setup_helper helper; }

class tool_main_test : public testing::Test {
 public:
  void SetUp() override {
    setup_locale();
    iol = std::make_unique<test::test_iolayer>();
  }

  void TearDown() override { iol.reset(); }

  std::string out() const { return iol->out(); }
  std::string err() const { return iol->err(); }

  std::unique_ptr<test::test_iolayer> iol;
};

class tester_common {
 public:
  using main_ptr_t = int (*)(std::span<std::string>, iolayer const&);

  tester_common(main_ptr_t mp, std::string toolname,
                std::shared_ptr<test::os_access_mock> pos)
      : fa{std::make_shared<test::test_file_access>()}
      , os{std::move(pos)}
      , iol{std::make_unique<test::test_iolayer>(os, fa)}
      , main_{mp}
      , toolname_{std::move(toolname)} {
    setup_locale();
  }

  int run(std::vector<std::string> args) {
    args.insert(args.begin(), toolname_);
    return main_(args, iol->get());
  }

  int run(std::initializer_list<std::string> args) {
    return run(std::vector<std::string>(args));
  }

  int run(std::string args) { return run(test::parse_args(args)); }

  std::string out() const { return iol->out(); }
  std::string err() const { return iol->err(); }

  std::shared_ptr<test::test_file_access> fa;
  std::shared_ptr<test::os_access_mock> os;
  std::unique_ptr<test::test_iolayer> iol;

 private:
  main_ptr_t main_;
  std::string toolname_;
};

class mkdwarfs_tester : public tester_common {
 public:
  mkdwarfs_tester(std::shared_ptr<test::os_access_mock> pos)
      : tester_common(mkdwarfs_main, "mkdwarfs", std::move(pos)) {}

  mkdwarfs_tester()
      : mkdwarfs_tester(test::os_access_mock::create_test_instance()) {}

  static mkdwarfs_tester create_empty() {
    return mkdwarfs_tester(std::make_shared<test::os_access_mock>());
  }

  void add_stream_logger(std::ostream& os,
                         logger::level_type level = logger::VERBOSE) {
    lgr = std::make_unique<stream_logger>(
        std::make_shared<test::test_terminal>(os, os), os, level);
  }

  void add_root_dir() { os->add("", {1, 040755, 1, 0, 0, 10, 42, 0, 0, 0}); }

  void add_random_file_tree(double avg_size = 4096.0, int dimension = 20) {
    size_t max_size{128 * static_cast<size_t>(avg_size)};
    std::mt19937_64 rng{42};
    std::exponential_distribution<> size_dist{1 / avg_size};

    for (int x = 0; x < dimension; ++x) {
      os->add_dir(fmt::format("{}", x));
      for (int y = 0; y < dimension; ++y) {
        os->add_dir(fmt::format("{}/{}", x, y));
        for (int z = 0; z < dimension; ++z) {
          auto size = std::min(max_size, static_cast<size_t>(size_dist(rng)));
          os->add_file(fmt::format("{}/{}/{}", x, y, z), size, true);
        }
      }
    }
  }

  void add_test_file_tree() {
    for (auto const& [stat, name] : test::test_dirtree()) {
      auto path = name.substr(name.size() == 5 ? 5 : 6);

      switch (stat.type()) {
      case posix_file_type::regular:
        os->add(path, stat,
                [size = stat.size] { return test::loremipsum(size); });
        break;
      case posix_file_type::symlink:
        os->add(path, stat, test::loremipsum(stat.size));
        break;
      default:
        os->add(path, stat);
        break;
      }
    }
  }

  filesystem_v2
  fs_from_data(std::string data, filesystem_options const& opt = {}) {
    if (!lgr) {
      lgr = std::make_unique<test::test_logger>();
    }
    auto mm = std::make_shared<test::mmap_mock>(std::move(data));
    return filesystem_v2(*lgr, mm, opt);
  }

  filesystem_v2 fs_from_file(std::string path) {
    auto fsimage = fa->get_file(path);
    if (!fsimage) {
      throw std::runtime_error("file not found: " + path);
    }
    return fs_from_data(std::move(fsimage.value()));
  }

  filesystem_v2 fs_from_stdout(filesystem_options const& opt = {}) {
    return fs_from_data(out(), opt);
  }

  std::unique_ptr<logger> lgr;
};

std::string build_test_image() {
  mkdwarfs_tester t;
  if (t.run({"-i", "/", "-o", "-"}) != 0) {
    throw std::runtime_error("failed to build test image:\n" + t.err());
  }
  return t.out();
}

class dwarfsck_tester : public tester_common {
 public:
  dwarfsck_tester(std::shared_ptr<test::os_access_mock> pos)
      : tester_common(dwarfsck_main, "dwarfsck", std::move(pos)) {}

  dwarfsck_tester()
      : dwarfsck_tester(std::make_shared<test::os_access_mock>()) {}

  static dwarfsck_tester create_with_image(std::string image) {
    auto os = std::make_shared<test::os_access_mock>();
    os->add("", {1, 040755, 1, 0, 0, 10, 42, 0, 0, 0});
    os->add_file("image.dwarfs", std::move(image));
    return dwarfsck_tester(std::move(os));
  }

  static dwarfsck_tester create_with_image() {
    return create_with_image(build_test_image());
  }
};

class dwarfsextract_tester : public tester_common {
 public:
  dwarfsextract_tester(std::shared_ptr<test::os_access_mock> pos)
      : tester_common(dwarfsextract_main, "dwarfsextract", std::move(pos)) {}

  dwarfsextract_tester()
      : dwarfsextract_tester(std::make_shared<test::os_access_mock>()) {}

  static dwarfsextract_tester create_with_image(std::string image) {
    auto os = std::make_shared<test::os_access_mock>();
    os->add("", {1, 040755, 1, 0, 0, 10, 42, 0, 0, 0});
    os->add_file("image.dwarfs", std::move(image));
    return dwarfsextract_tester(std::move(os));
  }

  static dwarfsextract_tester create_with_image() {
    return create_with_image(build_test_image());
  }
};

std::tuple<std::optional<filesystem_v2>, mkdwarfs_tester>
build_with_args(std::vector<std::string> opt_args = {}) {
  std::string const image_file = "test.dwarfs";
  mkdwarfs_tester t;
  std::vector<std::string> args = {"-i", "/", "-o", image_file};
  args.insert(args.end(), opt_args.begin(), opt_args.end());
  if (t.run(args) != 0) {
    return {std::nullopt, std::move(t)};
  }
  return {t.fs_from_file(image_file), std::move(t)};
}

std::set<uint64_t> get_all_fs_times(filesystem_v2 const& fs) {
  std::set<uint64_t> times;
  fs.walk([&](auto const& e) {
    file_stat st;
    fs.getattr(e.inode(), &st);
    times.insert(st.atime);
    times.insert(st.ctime);
    times.insert(st.mtime);
  });
  return times;
}

std::set<uint64_t> get_all_fs_uids(filesystem_v2 const& fs) {
  std::set<uint64_t> uids;
  fs.walk([&](auto const& e) {
    file_stat st;
    fs.getattr(e.inode(), &st);
    uids.insert(st.uid);
  });
  return uids;
}

std::set<uint64_t> get_all_fs_gids(filesystem_v2 const& fs) {
  std::set<uint64_t> gids;
  fs.walk([&](auto const& e) {
    file_stat st;
    fs.getattr(e.inode(), &st);
    gids.insert(st.gid);
  });
  return gids;
}

} // namespace

class mkdwarfs_main_test : public tool_main_test {
 public:
  int run(std::vector<std::string> args) {
    args.insert(args.begin(), "mkdwarfs");
    return mkdwarfs_main(args, iol->get());
  }
};

class dwarfsck_main_test : public tool_main_test {
 public:
  int run(std::vector<std::string> args) {
    args.insert(args.begin(), "dwarfsck");
    return dwarfsck_main(args, iol->get());
  }
};

class dwarfsextract_main_test : public tool_main_test {
 public:
  int run(std::vector<std::string> args) {
    args.insert(args.begin(), "dwarfsextract");
    return dwarfsextract_main(args, iol->get());
  }
};

TEST_F(mkdwarfs_main_test, no_cmdline_args) {
  auto exit_code = run({});
  EXPECT_EQ(exit_code, 0);
  EXPECT_TRUE(err().empty());
  EXPECT_FALSE(out().empty());
  EXPECT_THAT(out(), ::testing::HasSubstr("Usage: mkdwarfs"));
  EXPECT_THAT(out(), ::testing::HasSubstr("--help"));
}

TEST_F(dwarfsck_main_test, no_cmdline_args) {
  auto exit_code = run({});
  EXPECT_EQ(exit_code, 0);
  EXPECT_TRUE(err().empty());
  EXPECT_FALSE(out().empty());
  EXPECT_THAT(out(), ::testing::HasSubstr("Usage: dwarfsck"));
  EXPECT_THAT(out(), ::testing::HasSubstr("--help"));
}

TEST_F(dwarfsextract_main_test, no_cmdline_args) {
  auto exit_code = run({});
  EXPECT_EQ(exit_code, 0);
  EXPECT_TRUE(err().empty());
  EXPECT_FALSE(out().empty());
  EXPECT_THAT(out(), ::testing::HasSubstr("Usage: dwarfsextract"));
  EXPECT_THAT(out(), ::testing::HasSubstr("--help"));
}

TEST_F(mkdwarfs_main_test, invalid_cmdline_args) {
  auto exit_code = run({"--some-invalid-option"});
  EXPECT_EQ(exit_code, 1);
  EXPECT_FALSE(err().empty());
  EXPECT_TRUE(out().empty());
  EXPECT_THAT(err(), ::testing::HasSubstr(
                         "unrecognised option '--some-invalid-option'"));
}

TEST_F(dwarfsck_main_test, invalid_cmdline_args) {
  auto exit_code = run({"--some-invalid-option"});
  EXPECT_EQ(exit_code, 1);
  EXPECT_FALSE(err().empty());
  EXPECT_TRUE(out().empty());
  EXPECT_THAT(err(), ::testing::HasSubstr(
                         "unrecognised option '--some-invalid-option'"));
}

TEST_F(dwarfsextract_main_test, invalid_cmdline_args) {
  auto exit_code = run({"--some-invalid-option"});
  EXPECT_EQ(exit_code, 1);
  EXPECT_FALSE(err().empty());
  EXPECT_TRUE(out().empty());
  EXPECT_THAT(err(), ::testing::HasSubstr(
                         "unrecognised option '--some-invalid-option'"));
}

TEST_F(mkdwarfs_main_test, cmdline_help_arg) {
  auto exit_code = run({"--help"});
  EXPECT_EQ(exit_code, 0);
  EXPECT_TRUE(err().empty());
  EXPECT_FALSE(out().empty());
  EXPECT_THAT(out(), ::testing::HasSubstr("Usage: mkdwarfs"));
  EXPECT_THAT(out(), ::testing::HasSubstr("--help"));
  EXPECT_THAT(out(), ::testing::HasSubstr("--long-help"));
  // check that the detailed help is not shown
  EXPECT_THAT(out(), ::testing::Not(::testing::HasSubstr("Advanced options:")));
  EXPECT_THAT(out(),
              ::testing::Not(::testing::HasSubstr("Compression algorithms:")));
}

TEST_F(mkdwarfs_main_test, cmdline_long_help_arg) {
  auto exit_code = run({"--long-help"});
  EXPECT_EQ(exit_code, 0);
  EXPECT_TRUE(err().empty());
  EXPECT_FALSE(out().empty());
  EXPECT_THAT(out(), ::testing::HasSubstr("Usage: mkdwarfs"));
  EXPECT_THAT(out(), ::testing::HasSubstr("Advanced options:"));
  EXPECT_THAT(out(), ::testing::HasSubstr("Compression level defaults:"));
  EXPECT_THAT(out(), ::testing::HasSubstr("Compression algorithms:"));
  EXPECT_THAT(out(), ::testing::HasSubstr("Categories:"));
}

TEST_F(dwarfsck_main_test, cmdline_help_arg) {
  auto exit_code = run({"--help"});
  EXPECT_EQ(exit_code, 0);
  EXPECT_TRUE(err().empty());
  EXPECT_FALSE(out().empty());
  EXPECT_THAT(out(), ::testing::HasSubstr("Usage: dwarfsck"));
}

TEST_F(dwarfsextract_main_test, cmdline_help_arg) {
  auto exit_code = run({"--help"});
  EXPECT_EQ(exit_code, 0);
  EXPECT_TRUE(err().empty());
  EXPECT_FALSE(out().empty());
  EXPECT_THAT(out(), ::testing::HasSubstr("Usage: dwarfsextract"));
}

#ifdef DWARFS_PERFMON_ENABLED
TEST(dwarfsextract_test, perfmon) {
  auto t = dwarfsextract_tester::create_with_image();
  EXPECT_EQ(0, t.run({"-i", "image.dwarfs", "-f", "mtree", "--perfmon",
                      "filesystem_v2,inode_reader_v2"}))
      << t.err();
  auto outs = t.out();
  auto errs = t.err();
  EXPECT_GT(outs.size(), 100);
  EXPECT_FALSE(errs.empty());
  EXPECT_THAT(errs, ::testing::HasSubstr("[filesystem_v2.readv_future]"));
  EXPECT_THAT(errs, ::testing::HasSubstr("[filesystem_v2.getattr]"));
  EXPECT_THAT(errs, ::testing::HasSubstr("[filesystem_v2.open]"));
  EXPECT_THAT(errs, ::testing::HasSubstr("[filesystem_v2.readlink]"));
  EXPECT_THAT(errs, ::testing::HasSubstr("[filesystem_v2.statvfs]"));
  EXPECT_THAT(errs, ::testing::HasSubstr("[inode_reader_v2.readv_future]"));
#ifndef _WIN32
  // googletest on Windows does not support fancy regexes
  EXPECT_THAT(errs, ::testing::ContainsRegex(
                        R"(\[filesystem_v2\.getattr\])"
                        R"(\s+samples:\s+[0-9]+)"
                        R"(\s+overall:\s+[0-9]+(\.[0-9]+)?[num]?s)"
                        R"(\s+avg latency:\s+[0-9]+(\.[0-9]+)?[num]?s)"
                        R"(\s+p50 latency:\s+[0-9]+(\.[0-9]+)?[num]?s)"
                        R"(\s+p90 latency:\s+[0-9]+(\.[0-9]+)?[num]?s)"
                        R"(\s+p99 latency:\s+[0-9]+(\.[0-9]+)?[num]?s)"));
#endif
}
#endif

class mkdwarfs_input_list_test : public testing::TestWithParam<input_mode> {};

TEST_P(mkdwarfs_input_list_test, basic) {
  auto mode = GetParam();
  std::string const image_file = "test.dwarfs";
  std::string const input_list = "somelink\nfoo.pl\nsomedir/ipsum.py\n";

  mkdwarfs_tester t;
  std::string input_file;

  if (mode == input_mode::from_file) {
    input_file = "input_list.txt";
    t.fa->set_file(input_file, input_list);
  } else {
    input_file = "-";
    t.iol->set_in(input_list);
  }

  EXPECT_EQ(0, t.run({"--input-list", input_file, "-o", image_file}));

  std::ostringstream oss;
  t.add_stream_logger(oss, logger::DEBUG);

  auto fs = t.fs_from_file(image_file);

  auto link = fs.find("/somelink");
  auto foo = fs.find("/foo.pl");
  auto ipsum = fs.find("/somedir/ipsum.py");

  EXPECT_TRUE(link);
  EXPECT_TRUE(foo);
  EXPECT_TRUE(ipsum);

  EXPECT_FALSE(fs.find("/test.pl"));

  EXPECT_TRUE(link->is_symlink());
  EXPECT_TRUE(foo->is_regular_file());
  EXPECT_TRUE(ipsum->is_regular_file());

  std::set<fs::path> const expected = {"", "somelink", "foo.pl", "somedir",
                                       fs::path("somedir") / "ipsum.py"};
  std::set<fs::path> actual;
  fs.walk([&](auto const& e) { actual.insert(e.fs_path()); });

  EXPECT_EQ(expected, actual);
}

INSTANTIATE_TEST_SUITE_P(dwarfs, mkdwarfs_input_list_test,
                         ::testing::ValuesIn(input_modes));

class categorizer_test : public testing::TestWithParam<std::string> {};

TEST_P(categorizer_test, end_to_end) {
  auto level = GetParam();
  std::string const image_file = "test.dwarfs";

  auto t = mkdwarfs_tester::create_empty();

  t.add_root_dir();
  t.os->add_local_files(audio_data_dir);
  t.os->add_file("random", 4096, true);

  EXPECT_EQ(0, t.run({"-i", "/", "-o", image_file, "--categorize",
                      "--log-level=" + level}));

  auto fs = t.fs_from_file(image_file);

  auto iv16 = fs.find("/test8.aiff");
  auto iv32 = fs.find("/test8.caf");

  EXPECT_TRUE(iv16);
  EXPECT_TRUE(iv32);

  {
    std::vector<std::string> dumps;

    for (int detail = 0; detail <= 6; ++detail) {
      std::ostringstream os;
      fs.dump(os, detail);
      auto d = os.str();
      if (!dumps.empty()) {
        EXPECT_GT(d.size(), dumps.back().size()) << detail;
      }
      dumps.emplace_back(std::move(d));
    }

    EXPECT_GT(dumps.back().size(), 10'000);
  }

  {
    std::vector<std::string> infos;

    for (int detail = 0; detail <= 4; ++detail) {
      auto info = fs.info_as_dynamic(detail);
      auto i = folly::toJson(info);
      if (!infos.empty()) {
        EXPECT_GT(i.size(), infos.back().size()) << detail;
      }
      infos.emplace_back(std::move(i));
    }

    EXPECT_GT(infos.back().size(), 1'000);
  }
}

INSTANTIATE_TEST_SUITE_P(dwarfs, categorizer_test,
                         ::testing::Values("error", "warn", "info", "verbose",
                                           "debug", "trace"));

TEST(mkdwarfs_test, chmod_norm) {
  std::string const image_file = "test.dwarfs";

  std::set<std::string> real, norm;

  {
    mkdwarfs_tester t;
    EXPECT_EQ(0, t.run({"-i", "/", "-o", image_file}));
    auto fs = t.fs_from_file(image_file);
    fs.walk([&](auto const& e) { real.insert(e.inode().perm_string()); });
  }

  {
    mkdwarfs_tester t;
    EXPECT_EQ(0, t.run({"-i", "/", "-o", image_file, "--chmod=norm"}));
    auto fs = t.fs_from_file(image_file);
    fs.walk([&](auto const& e) { norm.insert(e.inode().perm_string()); });
  }

  EXPECT_NE(real, norm);

  std::set<std::string> expected_norm = {"r--r--r--", "r-xr-xr-x"};

  EXPECT_EQ(expected_norm, norm);
}

TEST(mkdwarfs_test, dump_inodes) {
  std::string const image_file = "test.dwarfs";
  std::string const inode_file = "inode.dump";

  auto t = mkdwarfs_tester::create_empty();
  t.add_root_dir();
  t.os->add_local_files(audio_data_dir);
  t.os->add_file("random", 4096, true);
  t.os->add_file("large", 32 * 1024 * 1024);
  t.add_random_file_tree(1024, 8);
  t.os->setenv("DWARFS_DUMP_INODES", inode_file);

  EXPECT_EQ(0, t.run({"-i", "/", "-o", image_file, "--categorize", "-W8"}));

  auto dump = t.fa->get_file(inode_file);

  ASSERT_TRUE(dump);
  EXPECT_GT(dump->size(), 1000) << dump.value();
}

TEST(mkdwarfs_test, set_time_now) {
  auto t0 =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

  auto [regfs, regt] = build_with_args();
  ASSERT_TRUE(regfs) << regt.err();
  auto reg = get_all_fs_times(*regfs);

  auto [optfs, optt] = build_with_args({"--set-time=now"});
  ASSERT_TRUE(optfs) << optt.err();
  auto opt = get_all_fs_times(*optfs);

  auto t1 =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

  ASSERT_EQ(reg.size(), 11);
  ASSERT_EQ(opt.size(), 1);

  EXPECT_GE(*opt.begin(), t0);
  EXPECT_LE(*opt.begin(), t1);
}

TEST(mkdwarfs_test, set_time_epoch) {
  auto [regfs, regt] = build_with_args();
  ASSERT_TRUE(regfs) << regt.err();
  auto reg = get_all_fs_times(*regfs);

  auto [optfs, optt] = build_with_args({"--set-time=100000001"});
  ASSERT_TRUE(optfs) << optt.err();
  auto opt = get_all_fs_times(*optfs);

  EXPECT_EQ(reg.size(), 11);
  ASSERT_EQ(opt.size(), 1);

  EXPECT_EQ(*opt.begin(), 100000001);
}

TEST(mkdwarfs_test, set_time_epoch_string) {
  using namespace std::chrono_literals;
  using std::chrono::sys_days;

  auto [optfs, optt] = build_with_args({"--set-time", "2020-01-01 01:02"});
  ASSERT_TRUE(optfs) << optt.err();
  auto opt = get_all_fs_times(*optfs);

  ASSERT_EQ(opt.size(), 1);

  EXPECT_EQ(*opt.begin(),
            std::chrono::duration_cast<std::chrono::seconds>(
                (sys_days{2020y / 1 / 1} + 1h + 2min).time_since_epoch())
                .count());
}

TEST(mkdwarfs_test, set_time_error) {
  auto t = mkdwarfs_tester::create_empty();
  EXPECT_NE(0, t.run({"-i", "/", "-o", "-", "--set-time=InVaLiD"}));
  EXPECT_THAT(t.err(), ::testing::HasSubstr("cannot parse time point"));
}

TEST(mkdwarfs_test, set_owner) {
  auto [regfs, regt] = build_with_args();
  ASSERT_TRUE(regfs) << regt.err();
  auto reg = get_all_fs_uids(*regfs);

  auto [optfs, optt] = build_with_args({"--set-owner=333"});
  ASSERT_TRUE(optfs) << optt.err();
  auto opt = get_all_fs_uids(*optfs);

  ASSERT_EQ(reg.size(), 2);
  ASSERT_EQ(opt.size(), 1);

  EXPECT_EQ(*opt.begin(), 333);
}

TEST(mkdwarfs_test, set_group) {
  auto [regfs, regt] = build_with_args();
  ASSERT_TRUE(regfs) << regt.err();
  auto reg = get_all_fs_gids(*regfs);

  auto [optfs, optt] = build_with_args({"--set-group=444"});
  ASSERT_TRUE(optfs) << optt.err();
  auto opt = get_all_fs_gids(*optfs);

  ASSERT_EQ(reg.size(), 2);
  ASSERT_EQ(opt.size(), 1);

  EXPECT_EQ(*opt.begin(), 444);
}

TEST(mkdwarfs_test, unrecognized_arguments) {
  auto t = mkdwarfs_tester::create_empty();
  EXPECT_NE(0, t.run({"grmpf"}));
  EXPECT_THAT(t.err(), ::testing::HasSubstr("unrecognized argument"));
}

TEST(mkdwarfs_test, invalid_compression_level) {
  auto t = mkdwarfs_tester::create_empty();
  EXPECT_NE(0, t.run({"-i", "/", "-o", "-", "-l", "10"}));
  EXPECT_THAT(t.err(), ::testing::HasSubstr("invalid compression level"));
}

TEST(mkdwarfs_test, block_size_too_small) {
  auto t = mkdwarfs_tester::create_empty();
  EXPECT_NE(0, t.run({"-i", "/", "-o", "-", "-S", "1"}));
  EXPECT_THAT(t.err(), ::testing::HasSubstr("block size must be between"));
}

TEST(mkdwarfs_test, block_size_too_large) {
  auto t = mkdwarfs_tester::create_empty();
  EXPECT_NE(0, t.run({"-i", "/", "-o", "-", "-S", "100"}));
  EXPECT_THAT(t.err(), ::testing::HasSubstr("block size must be between"));
}

TEST(mkdwarfs_test, cannot_combine_input_list_and_filter) {
  auto t = mkdwarfs_tester::create_empty();
  EXPECT_NE(0, t.run({"--input-list", "-", "-o", "-", "-F", "+ *"}));
  EXPECT_THAT(t.err(),
              ::testing::HasSubstr("cannot combine --input-list and --filter"));
}

TEST(mkdwarfs_test, cannot_open_input_list_file) {
  mkdwarfs_tester t;
  EXPECT_NE(0, t.run({"--input-list", "missing.list", "-o", "-"}));
  EXPECT_THAT(t.err(), ::testing::HasSubstr("cannot open input list file"));
}

TEST(mkdwarfs_test, recompress) {
  std::string const image_file = "test.dwarfs";
  std::string image;

  {
    mkdwarfs_tester t;
    t.os->add_local_files(audio_data_dir);
    t.os->add_file("random", 4096, true);
    ASSERT_EQ(0, t.run({"-i", "/", "-o", image_file, "--categorize"}))
        << t.err();
    auto img = t.fa->get_file(image_file);
    EXPECT_TRUE(img);
    image = std::move(img.value());
  }

  auto tester = [&image_file](std::string const& image_data) {
    auto t = mkdwarfs_tester::create_empty();
    t.add_root_dir();
    t.os->add_file(image_file, image_data);
    return t;
  };

  {
    auto t = tester(image);
    ASSERT_EQ(0, t.run({"-i", image_file, "-o", "-", "--recompress", "-l0"}))
        << t.err();
    auto fs = t.fs_from_stdout();
    EXPECT_TRUE(fs.find("/random"));
  }

  {
    auto t = tester(image);
    EXPECT_NE(0, t.run({"-i", image_file, "-o", "-", "--recompress=foo"}));
    EXPECT_THAT(t.err(), ::testing::HasSubstr("invalid recompress mode"));
  }

  {
    auto t = tester(image);
    ASSERT_EQ(0, t.run({"-i", image_file, "-o", "-", "--recompress=metadata"}))
        << t.err();
    auto fs = t.fs_from_stdout();
    EXPECT_TRUE(fs.find("/random"));
  }

  {
    auto t = tester(image);
    ASSERT_EQ(0, t.run({"-i", image_file, "-o", "-", "--recompress=block",
                        "--recompress-categories=!pcmaudio/waveform", "-C",
                        "pcmaudio/metadata::null"}))
        << t.err();
    auto fs = t.fs_from_stdout();
    EXPECT_TRUE(fs.find("/random"));
  }

  {
    auto corrupt_image = image;
    corrupt_image[64] ^= 0x01; // flip a bit right after the header
    auto t = tester(corrupt_image);
    EXPECT_NE(0, t.run({"-i", image_file, "-o", "-", "--recompress"}))
        << t.err();
    EXPECT_THAT(t.err(), ::testing::HasSubstr("input filesystem is corrupt"));
  }
}

class mkdwarfs_build_options_test
    : public testing::TestWithParam<std::string_view> {};

TEST_P(mkdwarfs_build_options_test, basic) {
  auto opts = GetParam();
  auto options = test::parse_args(opts);
  std::string const image_file = "test.dwarfs";

  std::vector<std::string> args = {"-i", "/", "-o", image_file};
  args.insert(args.end(), options.begin(), options.end());

  auto t = mkdwarfs_tester::create_empty();

  t.add_root_dir();
  t.add_random_file_tree();
  t.os->add_local_files(audio_data_dir);

  EXPECT_EQ(0, t.run(args));

  auto fs = t.fs_from_file(image_file);
}

namespace {

constexpr std::array<std::string_view, 7> const build_options = {
    "--categorize --order=none --file-hash=none",
    "--categorize=pcmaudio --order=path",
    "--categorize --order=revpath --file-hash=sha512",
    "--categorize=pcmaudio,incompressible --order=similarity",
    "--categorize --order=nilsimsa --time-resolution=30",
    "--categorize --order=nilsimsa:max-children=1k --time-resolution=hour",
    "--categorize --order=nilsimsa:max-cluster-size=16:max-children=16 "
    "--max-similarity-size=1M",
};

} // namespace

INSTANTIATE_TEST_SUITE_P(dwarfs, mkdwarfs_build_options_test,
                         ::testing::ValuesIn(build_options));

TEST(mkdwarfs_test, order_invalid) {
  mkdwarfs_tester t;
  EXPECT_NE(0, t.run({"-i", "/", "-o", "-", "--order=grmpf"}));
  EXPECT_THAT(t.err(), ::testing::HasSubstr("invalid inode order mode"));
}

TEST(mkdwarfs_test, order_nilsimsa_invalid_option) {
  mkdwarfs_tester t;
  EXPECT_NE(0, t.run({"-i", "/", "-o", "-", "--order=nilsimsa:grmpf"}));
  EXPECT_THAT(t.err(), ::testing::HasSubstr(
                           "invalid option(s) for choice nilsimsa: grmpf"));
}

TEST(mkdwarfs_test, order_nilsimsa_invalid_value) {
  mkdwarfs_tester t;
  EXPECT_NE(0,
            t.run({"-i", "/", "-o", "-", "--order=nilsimsa:max-children=0"}));
  EXPECT_THAT(t.err(), ::testing::HasSubstr("invalid max-children value: 0"));
}

TEST(mkdwarfs_test, order_nilsimsa_cannot_parse_value) {
  mkdwarfs_tester t;
  EXPECT_NE(
      0, t.run({"-i", "/", "-o", "-", "--order=nilsimsa:max-cluster-size=-1"}));
  EXPECT_THAT(t.err(), ::testing::HasSubstr("cannot parse size value"));
}

TEST(mkdwarfs_test, order_nilsimsa_duplicate_option) {
  mkdwarfs_tester t;
  EXPECT_NE(0,
            t.run({"-i", "/", "-o", "-",
                   "--order=nilsimsa:max-cluster-size=1:max-cluster-size=10"}));
  EXPECT_THAT(t.err(),
              ::testing::HasSubstr(
                  "duplicate option max-cluster-size for choice nilsimsa"));
}

TEST(mkdwarfs_test, unknown_file_hash) {
  mkdwarfs_tester t;
  EXPECT_NE(0, t.run({"-i", "/", "-o", "-", "--file-hash=grmpf"}));
  EXPECT_THAT(t.err(), ::testing::HasSubstr("unknown file hash function"));
}

TEST(mkdwarfs_test, invalid_filter_debug_mode) {
  mkdwarfs_tester t;
  EXPECT_NE(0, t.run({"-i", "/", "-o", "-", "--debug-filter=grmpf"}));
  EXPECT_THAT(t.err(), ::testing::HasSubstr("invalid filter debug mode"));
}

TEST(mkdwarfs_test, invalid_progress_mode) {
  mkdwarfs_tester t;
  t.iol->set_terminal_fancy(true);
  EXPECT_NE(0, t.run({"-i", "/", "-o", "-", "--progress=grmpf"}));
  EXPECT_THAT(t.err(), ::testing::HasSubstr("invalid progress mode"));
}

TEST(mkdwarfs_test, invalid_filter_rule) {
  mkdwarfs_tester t;
  EXPECT_NE(0, t.run({"-i", "/", "-o", "-", "-F", "grmpf"}));
  EXPECT_THAT(t.err(), ::testing::HasSubstr("could not parse filter rule"));
}

TEST(mkdwarfs_test, time_resolution_zero) {
  mkdwarfs_tester t;
  EXPECT_NE(0, t.run({"-i", "/", "-o", "-", "--time-resolution=0"}));
  EXPECT_THAT(t.err(),
              ::testing::HasSubstr("'--time-resolution' must be nonzero"));
}

TEST(mkdwarfs_test, time_resolution_invalid) {
  mkdwarfs_tester t;
  EXPECT_NE(0, t.run({"-i", "/", "-o", "-", "--time-resolution=grmpf"}));
  EXPECT_THAT(t.err(), ::testing::HasSubstr("'--time-resolution' is invalid"));
}

namespace {

constexpr std::array<std::string_view, 6> const debug_filter_mode_names = {
    "included", "excluded", "included-files", "excluded-files", "files", "all",
};

const std::map<std::string_view, debug_filter_mode> debug_filter_modes{
    {"included", debug_filter_mode::INCLUDED},
    {"included-files", debug_filter_mode::INCLUDED_FILES},
    {"excluded", debug_filter_mode::EXCLUDED},
    {"excluded-files", debug_filter_mode::EXCLUDED_FILES},
    {"files", debug_filter_mode::FILES},
    {"all", debug_filter_mode::ALL},
};

} // namespace

class filter_test : public testing::TestWithParam<
                        std::tuple<test::filter_test_data, std::string_view>> {
};

TEST_P(filter_test, debug_filter) {
  auto [data, mode] = GetParam();
  auto t = mkdwarfs_tester::create_empty();
  t.add_test_file_tree();
  t.fa->set_file("filter.txt", data.filter());
  ASSERT_EQ(0, t.run({"-i", "/", "-F", ". filter.txt",
                      "--debug-filter=" + std::string(mode)}))
      << t.err();
  auto expected = data.get_expected_filter_output(debug_filter_modes.at(mode));
  EXPECT_EQ(expected, t.out());
}

INSTANTIATE_TEST_SUITE_P(
    mkdwarfs_test, filter_test,
    ::testing::Combine(::testing::ValuesIn(dwarfs::test::get_filter_tests()),
                       ::testing::ValuesIn(debug_filter_mode_names)));

namespace {

constexpr std::array<std::string_view, 9> const pack_mode_names = {
    "chunk_table", "directories",    "shared_files", "names", "names_index",
    "symlinks",    "symlinks_index", "force",        "plain",
};

}

TEST(mkdwarfs_test, pack_modes_random) {
  std::mt19937_64 rng{42};
  std::uniform_int_distribution<> dist{1, pack_mode_names.size()};

  for (int i = 0; i < 50; ++i) {
    std::vector<std::string_view> modes(pack_mode_names.begin(),
                                        pack_mode_names.end());
    std::shuffle(modes.begin(), modes.end(), rng);
    modes.resize(dist(rng));
    auto mode_arg = folly::join(',', modes);
    auto t = mkdwarfs_tester::create_empty();
    t.add_test_file_tree();
    t.add_random_file_tree(128.0, 16);
    ASSERT_EQ(
        0, t.run({"-i", "/", "-o", "-", "-l1", "--pack-metadata=" + mode_arg}))
        << t.err();
    auto fs = t.fs_from_stdout();
    auto info = fs.info_as_dynamic(2);
    std::set<std::string> ms(modes.begin(), modes.end());
    std::set<std::string> fsopt;
    for (auto const& opt : info["options"]) {
      fsopt.insert(opt.asString());
    }
    auto ctx = mode_arg + "\n" + fs.dump(2);
    EXPECT_EQ(ms.count("chunk_table"), fsopt.count("packed_chunk_table"))
        << ctx;
    EXPECT_EQ(ms.count("directories"), fsopt.count("packed_directories"))
        << ctx;
    EXPECT_EQ(ms.count("shared_files"),
              fsopt.count("packed_shared_files_table"))
        << ctx;
    if (ms.count("plain")) {
      EXPECT_EQ(0, fsopt.count("packed_names")) << ctx;
      EXPECT_EQ(0, fsopt.count("packed_names_index")) << ctx;
      EXPECT_EQ(0, fsopt.count("packed_symlinks")) << ctx;
      EXPECT_EQ(0, fsopt.count("packed_symlinks_index")) << ctx;
    }
  }
}

TEST(mkdwarfs_test, pack_mode_none) {
  auto t = mkdwarfs_tester::create_empty();
  t.add_test_file_tree();
  t.add_random_file_tree(128.0, 16);
  ASSERT_EQ(0, t.run({"-i", "/", "-o", "-", "-l1", "--pack-metadata=none"}))
      << t.err();
  auto fs = t.fs_from_stdout();
  auto info = fs.info_as_dynamic(2);
  std::set<std::string> fsopt;
  for (auto const& opt : info["options"]) {
    fsopt.insert(opt.asString());
  }
  fsopt.erase("mtime_only");
  EXPECT_TRUE(fsopt.empty()) << folly::toJson(info["options"]);
}

TEST(mkdwarfs_test, pack_mode_all) {
  auto t = mkdwarfs_tester::create_empty();
  t.add_test_file_tree();
  t.add_random_file_tree(128.0, 16);
  ASSERT_EQ(0, t.run({"-i", "/", "-o", "-", "-l1", "--pack-metadata=all"}))
      << t.err();
  auto fs = t.fs_from_stdout();
  auto info = fs.info_as_dynamic(2);
  std::set<std::string> expected = {"packed_chunk_table",
                                    "packed_directories",
                                    "packed_names",
                                    "packed_names_index",
                                    "packed_shared_files_table",
                                    "packed_symlinks_index"};
  std::set<std::string> fsopt;
  for (auto const& opt : info["options"]) {
    fsopt.insert(opt.asString());
  }
  fsopt.erase("mtime_only");
  EXPECT_EQ(expected, fsopt) << folly::toJson(info["options"]);
}

TEST(mkdwarfs_test, pack_mode_invalid) {
  mkdwarfs_tester t;
  EXPECT_NE(0, t.run({"-i", "/", "-o", "-", "--pack-metadata=grmpf"}));
  EXPECT_THAT(t.err(), ::testing::HasSubstr("'--pack-metadata' is invalid"));
}

TEST(mkdwarfs_test, filesystem_header) {
  auto const header = test::loremipsum(333);

  mkdwarfs_tester t;
  t.fa->set_file("header.txt", header);
  ASSERT_EQ(0, t.run({"-i", "/", "-o", "-", "--header=header.txt"})) << t.err();

  auto image = t.out();

  auto fs = t.fs_from_data(
      image, {.image_offset = filesystem_options::IMAGE_OFFSET_AUTO});
  auto hdr = fs.header();
  ASSERT_TRUE(hdr);
  std::string actual(reinterpret_cast<char const*>(hdr->data()), hdr->size());
  EXPECT_EQ(header, actual);

  auto os = std::make_shared<test::os_access_mock>();
  os->add("", {1, 040755, 1, 0, 0, 10, 42, 0, 0, 0});
  os->add_file("image.dwarfs", image);

  {
    dwarfsck_tester t2(os);
    EXPECT_EQ(0, t2.run({"image.dwarfs", "--print-header"})) << t2.err();
    EXPECT_EQ(header, t2.out());
  }

  {
    mkdwarfs_tester t2(os);
    ASSERT_EQ(0, t2.run({"-i", "image.dwarfs", "-o", "-", "--recompress=none",
                         "--remove-header"}))
        << t2.err();

    auto fs2 = t2.fs_from_stdout();
    EXPECT_FALSE(fs2.header());
  }
}

TEST(mkdwarfs_test, filesystem_header_error) {
  mkdwarfs_tester t;
  EXPECT_NE(0, t.run({"-i", "/", "-o", "-", "--header=header.txt"})) << t.err();
  EXPECT_THAT(t.err(), ::testing::HasSubstr("cannot open header file"));
}

TEST(mkdwarfs_test, output_file_exists) {
  mkdwarfs_tester t;
  t.fa->set_file("exists.dwarfs", "bla");
  EXPECT_NE(0, t.run({"-i", "/", "-o", "exists.dwarfs"})) << t.err();
  EXPECT_THAT(t.err(), ::testing::HasSubstr("output file already exists"));
}

TEST(mkdwarfs_test, output_file_force) {
  mkdwarfs_tester t;
  t.fa->set_file("exists.dwarfs", "bla");
  EXPECT_EQ(0, t.run({"-i", "/", "-o", "exists.dwarfs", "-l1", "--force"}))
      << t.err();
  auto fs = t.fs_from_file("exists.dwarfs");
  EXPECT_TRUE(fs.find("/foo.pl"));
}

TEST(mkdwarfs_test, output_file_fail_open) {
  mkdwarfs_tester t;
  t.fa->set_file("exists.dwarfs", "bla");
  t.fa->set_open_error(
      "exists.dwarfs",
      std::make_error_code(std::errc::device_or_resource_busy));
  EXPECT_NE(0, t.run({"-i", "/", "-o", "exists.dwarfs", "--force"})) << t.err();
  EXPECT_THAT(t.err(), ::testing::HasSubstr("cannot open output file"));
}

TEST(mkdwarfs_test, output_file_fail_close) {
  mkdwarfs_tester t;
  t.fa->set_close_error("test.dwarfs",
                        std::make_error_code(std::errc::no_space_on_device));
  EXPECT_NE(0, t.run({"-i", "/", "-o", "test.dwarfs"})) << t.err();
  EXPECT_THAT(t.err(), ::testing::HasSubstr("failed to close output file"));
}

TEST(mkdwarfs_test, compression_cannot_be_used_without_category) {
  mkdwarfs_tester t;
  EXPECT_NE(0, t.run({"-i", "/", "-o", "-", "-C", "flac"}));
  EXPECT_THAT(t.err(),
              ::testing::HasSubstr("cannot be used without a category"));
}

TEST(mkdwarfs_test, compression_cannot_be_used_for_category) {
  mkdwarfs_tester t;
  EXPECT_NE(0, t.run({"-i", "/", "-o", "-", "--categorize", "-C",
                      "incompressible::flac"}));
  EXPECT_THAT(t.err(), ::testing::HasSubstr(
                           "cannot be used for category 'incompressible': "
                           "metadata requirements not met"));
}

class mkdwarfs_progress_test : public testing::TestWithParam<char const*> {};

TEST_P(mkdwarfs_progress_test, basic) {
  std::string mode{GetParam()};
  std::string const image_file = "test.dwarfs";

  std::vector<std::string> args{
      "-i", "/", "-o", image_file, "--file-hash=sha512", "--progress", mode};

  auto t = mkdwarfs_tester::create_empty();

  t.iol->set_terminal_fancy(true);

  t.add_root_dir();
  t.add_random_file_tree();
  t.os->add_local_files(audio_data_dir);

  EXPECT_EQ(0, t.run(args));
  EXPECT_TRUE(t.out().empty()) << t.out();
}

namespace {

constexpr std::array const progress_modes{
    "none",
    "simple",
    "ascii",
#ifndef _WIN32
    "unicode",
#endif
};

} // namespace

INSTANTIATE_TEST_SUITE_P(dwarfs, mkdwarfs_progress_test,
                         ::testing::ValuesIn(progress_modes));

TEST(dwarfsextract_test, mtree) {
  auto t = dwarfsextract_tester::create_with_image();
  EXPECT_EQ(0, t.run({"-i", "image.dwarfs", "-f", "mtree"})) << t.err();
  auto out = t.out();
  EXPECT_TRUE(out.starts_with("#mtree")) << out;
  EXPECT_THAT(out, ::testing::HasSubstr("type=dir"));
  EXPECT_THAT(out, ::testing::HasSubstr("type=file"));
}

TEST(dwarfsextract_test, stdout_progress_error) {
  auto t = dwarfsextract_tester::create_with_image();
  EXPECT_NE(0,
            t.run({"-i", "image.dwarfs", "-f", "mtree", "--stdout-progress"}))
      << t.err();
  EXPECT_THAT(t.err(), ::testing::HasSubstr(
                           "cannot use --stdout-progress with --output=-"));
}

TEST(dwarfsck_test, check_exclusive) {
  auto t = dwarfsck_tester::create_with_image();
  EXPECT_NE(0, t.run({"image.dwarfs", "--no-check", "--check-integrity"}))
      << t.err();
  EXPECT_THAT(t.err(),
              ::testing::HasSubstr(
                  "--no-check and --check-integrity are mutually exclusive"));
}

TEST(dwarfsck_test, print_header_and_json) {
  auto t = dwarfsck_tester::create_with_image();
  EXPECT_NE(0, t.run({"image.dwarfs", "--print-header", "--json"})) << t.err();
  EXPECT_THAT(t.err(), ::testing::HasSubstr(
                           "--print-header is mutually exclusive with --json, "
                           "--export-metadata and --check-integrity"));
}

TEST(dwarfsck_test, print_header_and_export_metadata) {
  auto t = dwarfsck_tester::create_with_image();
  EXPECT_NE(0, t.run({"image.dwarfs", "--print-header",
                      "--export-metadata=image.meta"}))
      << t.err();
  EXPECT_THAT(t.err(), ::testing::HasSubstr(
                           "--print-header is mutually exclusive with --json, "
                           "--export-metadata and --check-integrity"));
}

TEST(dwarfsck_test, print_header_and_check_integrity) {
  auto t = dwarfsck_tester::create_with_image();
  EXPECT_NE(0, t.run({"image.dwarfs", "--print-header", "--check-integrity"}))
      << t.err();
  EXPECT_THAT(t.err(), ::testing::HasSubstr(
                           "--print-header is mutually exclusive with --json, "
                           "--export-metadata and --check-integrity"));
}

TEST(dwarfsck_test, print_header_no_header) {
  auto t = dwarfsck_tester::create_with_image();
  EXPECT_EQ(2, t.run({"image.dwarfs", "--print-header"})) << t.err();
  EXPECT_THAT(t.err(),
              ::testing::HasSubstr("filesystem does not contain a header"));
}

TEST(dwarfsck_test, export_metadata) {
  auto t = dwarfsck_tester::create_with_image();
  EXPECT_EQ(0, t.run({"image.dwarfs", "--export-metadata=image.meta"}))
      << t.err();
  auto meta = t.fa->get_file("image.meta");
  ASSERT_TRUE(meta);
  EXPECT_GT(meta->size(), 1000);
  EXPECT_NO_THROW(folly::parseJson(meta.value()));
}

TEST(dwarfsck_test, export_metadata_open_error) {
  auto t = dwarfsck_tester::create_with_image();
  t.fa->set_open_error(
      "image.meta", std::make_error_code(std::errc::device_or_resource_busy));
  EXPECT_NE(0, t.run({"image.dwarfs", "--export-metadata=image.meta"}))
      << t.err();
  EXPECT_THAT(t.err(),
              ::testing::HasSubstr("failed to open metadata output file"));
}

TEST(dwarfsck_test, export_metadata_close_error) {
  auto t = dwarfsck_tester::create_with_image();
  t.fa->set_close_error("image.meta",
                        std::make_error_code(std::errc::no_space_on_device));
  EXPECT_NE(0, t.run({"image.dwarfs", "--export-metadata=image.meta"}))
      << t.err();
  EXPECT_THAT(t.err(),
              ::testing::HasSubstr("failed to close metadata output file"));
}

class mkdwarfs_sim_order_test : public testing::TestWithParam<char const*> {};

TEST(mkdwarfs_test, max_similarity_size) {
  static constexpr std::array sizes{50, 100, 200, 500, 1000, 2000, 5000, 10000};

  auto make_tester = [] {
    std::mt19937_64 rng{42};
    auto t = mkdwarfs_tester::create_empty();

    t.add_root_dir();

    for (auto size : sizes) {
      auto data = test::create_random_string(size, rng);
      t.os->add_file("/file" + std::to_string(size), data);
    }

    return t;
  };

  auto get_sizes_in_offset_order = [](filesystem_v2 const& fs) {
    std::vector<std::pair<size_t, size_t>> tmp;

    for (auto size : sizes) {
      auto path = "/file" + std::to_string(size);
      auto iv = fs.find(path.c_str());
      assert(iv);
      auto info = fs.get_inode_info(*iv);
      assert(1 == info["chunks"].size());
      auto const& chunk = info["chunks"][0];
      tmp.emplace_back(chunk["offset"].asInt(), chunk["size"].asInt());
    }

    std::sort(tmp.begin(), tmp.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });

    std::vector<size_t> sizes;

    std::transform(tmp.begin(), tmp.end(), std::back_inserter(sizes),
                   [](auto const& p) { return p.second; });

    return sizes;
  };

  auto partitioned_sizes = [&](std::vector<size_t> in, size_t max_size) {
    auto mid = std::stable_partition(
        in.begin(), in.end(), [=](auto size) { return size > max_size; });

    std::sort(in.begin(), mid, std::greater<size_t>());

    return in;
  };

  std::vector<size_t> sim_ordered_sizes;
  std::vector<size_t> nilsimsa_ordered_sizes;

  {
    auto t = make_tester();
    EXPECT_EQ(0, t.run("-i / -o - -l0 --order=similarity")) << t.err();
    auto fs = t.fs_from_stdout();
    sim_ordered_sizes = get_sizes_in_offset_order(fs);
  }

  {
    auto t = make_tester();
    EXPECT_EQ(0, t.run("-i / -o - -l0 --order=nilsimsa")) << t.err();
    auto fs = t.fs_from_stdout();
    nilsimsa_ordered_sizes = get_sizes_in_offset_order(fs);
  }

  EXPECT_FALSE(
      std::is_sorted(sim_ordered_sizes.begin(), sim_ordered_sizes.end()));

  static constexpr std::array max_sim_sizes{0,    1,    200,  999,
                                            1000, 1001, 5000, 10000};

  std::set<std::string> nilsimsa_results;

  for (auto max_sim_size : max_sim_sizes) {
    {
      auto t = make_tester();
      EXPECT_EQ(0,
                t.run(fmt::format(
                    "-i / -o - -l0 --order=similarity --max-similarity-size={}",
                    max_sim_size)))
          << t.err();

      auto fs = t.fs_from_stdout();

      auto ordered_sizes = get_sizes_in_offset_order(fs);

      if (max_sim_size == 0) {
        EXPECT_EQ(sim_ordered_sizes, ordered_sizes) << max_sim_size;
      } else {
        auto partitioned = partitioned_sizes(sim_ordered_sizes, max_sim_size);
        EXPECT_EQ(partitioned, ordered_sizes) << max_sim_size;
      }
    }

    {
      auto t = make_tester();
      EXPECT_EQ(0,
                t.run(fmt::format(
                    "-i / -o - -l0 --order=nilsimsa --max-similarity-size={}",
                    max_sim_size)))
          << t.err();

      auto fs = t.fs_from_stdout();

      auto ordered_sizes = get_sizes_in_offset_order(fs);

      nilsimsa_results.insert(folly::join(",", ordered_sizes));

      if (max_sim_size == 0) {
        EXPECT_EQ(nilsimsa_ordered_sizes, ordered_sizes) << max_sim_size;
      } else {
        std::vector<size_t> expected;
        std::copy_if(sizes.begin(), sizes.end(), std::back_inserter(expected),
                     [=](auto size) { return size > max_sim_size; });
        std::sort(expected.begin(), expected.end(), std::greater<size_t>());
        ordered_sizes.resize(expected.size());
        EXPECT_EQ(expected, ordered_sizes) << max_sim_size;
      }
    }
  }

  EXPECT_GE(nilsimsa_results.size(), 3);
}