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

#include <cassert>
#include <regex>
#include <unordered_set>

#include <fmt/format.h>

#include <dwarfs/file_access.h>
#include <dwarfs/logger.h>
#include <dwarfs/util.h>
#include <dwarfs/writer/entry_interface.h>
#include <dwarfs/writer/rule_based_entry_filter.h>

namespace dwarfs::writer {

namespace internal {

namespace fs = std::filesystem;

namespace {

constexpr char const kLocalPathSeparator{
    static_cast<char>(fs::path::preferred_separator)};

}

struct filter_rule {
  enum class rule_type {
    include,
    exclude,
  };

  filter_rule(rule_type type, bool floating, std::string const& re,
              std::string const& rule)
      : type{type}
      , floating{floating}
      , re{re}
      , rule{rule} {}

  rule_type type;
  bool floating;
  std::regex re;
  std::string rule;
};

template <typename LoggerPolicy>
class rule_based_entry_filter_ : public rule_based_entry_filter::impl {
 public:
  rule_based_entry_filter_(logger& lgr, std::shared_ptr<file_access const> fa);

  void set_root_path(fs::path const& path) override;
  void add_rule(std::string_view rule) override;
  void add_rules(std::istream& is) override;
  filter_action filter(entry_interface const& ei) const override;

 private:
  void
  add_rule(std::unordered_set<std::string>& seen_files, std::string_view rule);

  void add_rules(std::unordered_set<std::string>& seen_files, std::istream& is);

  filter_rule compile_filter_rule(std::string_view rule);

  LOG_PROXY_DECL(LoggerPolicy);
  std::string root_path_;
  std::vector<filter_rule> filter_;
  std::shared_ptr<file_access const> fa_;
};

template <typename LoggerPolicy>
auto rule_based_entry_filter_<LoggerPolicy>::compile_filter_rule(
    std::string_view rule_sv) -> filter_rule {
  std::string rule{rule_sv};
  std::string re;
  filter_rule::rule_type type;

  auto* p = rule.c_str();

  switch (*p) {
  case '+':
    type = filter_rule::rule_type::include;
    break;
  case '-':
    type = filter_rule::rule_type::exclude;
    break;
  default:
    throw std::runtime_error("rules must start with + or -");
  }

  while (*++p == ' ')
    ;

  // If the start of the pattern is not explicitly anchored, make it floating.
  bool floating = *p && *p != '/';

  if (floating) {
    re += ".*/";
  }

  while (*p) {
    switch (*p) {
    case '\\':
      re += *p++;
      if (p) {
        re += *p++;
      }
      continue;

    case '*': {
      int nstar = 1;
      while (*++p == '*') {
        ++nstar;
      }
      switch (nstar) {
      case 1:
        if (re.ends_with('/') and (*p == '/' or *p == '\0')) {
          re += "[^/]+";
        } else {
          re += "[^/]*";
        }
        break;
      case 2:
        re += ".*";
        break;
      default:
        throw std::runtime_error("too many *s");
      }
    }
      continue;

    case '?':
      re += "[^/]";
      break;

    case '.':
    case '+':
    case '^':
    case '$':
    case '(':
    case ')':
    case '{':
    case '}':
    case '|':
      re += '\\';
      re += *p;
      break;

    default:
      re += *p;
      break;
    }

    ++p;
  }

  LOG_DEBUG << "'" << rule << "' -> '" << re << "' [floating=" << floating
            << "]";

  return filter_rule(type, floating, re, rule);
}

template <typename LoggerPolicy>
rule_based_entry_filter_<LoggerPolicy>::rule_based_entry_filter_(
    logger& lgr, std::shared_ptr<file_access const> fa)
    : log_{lgr}
    , fa_{std::move(fa)} {}

template <typename LoggerPolicy>
void rule_based_entry_filter_<LoggerPolicy>::set_root_path(
    fs::path const& path) {
  // TODO: this whole thing needs to be windowsized
  root_path_ = path_to_utf8_string_sanitized(path);

  if constexpr (kLocalPathSeparator != '/') {
    // Both '/' and '\\' are, surprisingly, valid path separators on Windows,
    // and invalid characters in filenames. So on Windows, it's a lossless
    // transformation to replace all '\\' with '/'.
    std::replace(root_path_.begin(), root_path_.end(), kLocalPathSeparator,
                 '/');
  }

  if (root_path_.ends_with('/')) {
    root_path_.pop_back();
  }
}

template <typename LoggerPolicy>
void rule_based_entry_filter_<LoggerPolicy>::add_rule(std::string_view rule) {
  std::unordered_set<std::string> seen_files;
  add_rule(seen_files, rule);
}

template <typename LoggerPolicy>
void rule_based_entry_filter_<LoggerPolicy>::add_rules(std::istream& is) {
  std::unordered_set<std::string> seen_files;
  add_rules(seen_files, is);
}

template <typename LoggerPolicy>
void rule_based_entry_filter_<LoggerPolicy>::add_rule(
    std::unordered_set<std::string>& seen_files, std::string_view rule) {
  if (rule.starts_with('.')) {
    auto file_pos = rule.find_first_not_of(" \t", 1);

    if (file_pos == std::string::npos) {
      throw std::runtime_error(
          fmt::format("no file specified in merge rule: {}", rule));
    }

    auto file = std::string(rule.substr(file_pos));

    if (!seen_files.emplace(file).second) {
      throw std::runtime_error(
          fmt::format("recursion detected while opening file: {}", file));
    }

    auto ifs = fa_->open_input(file);
    add_rules(seen_files, ifs->is());

    seen_files.erase(file);
  } else {
    filter_.push_back(compile_filter_rule(rule));
  }
}

template <typename LoggerPolicy>
void rule_based_entry_filter_<LoggerPolicy>::add_rules(
    std::unordered_set<std::string>& seen_files, std::istream& is) {
  std::string line;

  while (std::getline(is, line)) {
    if (line.starts_with('#')) {
      continue;
    }
    if (line.find_first_not_of(" \t") == std::string::npos) {
      continue;
    }
    add_rule(seen_files, line);
  }
}

template <typename LoggerPolicy>
filter_action rule_based_entry_filter_<LoggerPolicy>::filter(
    entry_interface const& ei) const {
  std::string path = ei.unix_dpath();
  std::string relpath = path;

  if (relpath.size() >= root_path_.size()) {
    assert(relpath.substr(0, root_path_.size()) == root_path_);
    relpath.erase(0, root_path_.size());
  }

  for (const auto& r : filter_) {
    if (std::regex_match(r.floating ? path : relpath, r.re)) {
      LOG_TRACE << "[" << path << "] / [" << relpath << "] matched rule '"
                << r.rule << "'";
      switch (r.type) {
      case filter_rule::rule_type::include:
        return filter_action::keep;

      case filter_rule::rule_type::exclude:
        return filter_action::remove;
      }
    }
  }

  LOG_TRACE << "[" << path << "] / [" << relpath << "] matched no rule";

  return filter_action::keep;
}

} // namespace internal

rule_based_entry_filter::rule_based_entry_filter(
    logger& lgr, std::shared_ptr<file_access const> fa)
    : impl_(make_unique_logging_object<impl, internal::rule_based_entry_filter_,
                                       logger_policies>(lgr, std::move(fa))) {}

rule_based_entry_filter::~rule_based_entry_filter() = default;

filter_action rule_based_entry_filter::filter(entry_interface const& ei) const {
  return impl_->filter(ei);
}

} // namespace dwarfs::writer
