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

#include <array>
#include <cassert>
#include <optional>

#include <lzma.h>

#include <fmt/format.h>

#include <range/v3/range/conversion.hpp>
#include <range/v3/view/join.hpp>
#include <range/v3/view/map.hpp>

#include <dwarfs/block_compressor.h>
#include <dwarfs/error.h>
#include <dwarfs/fstypes.h>
#include <dwarfs/option_map.h>
#include <dwarfs/sorted_array_map.h>
#include <dwarfs/types.h>

namespace dwarfs {

namespace {

using namespace std::string_view_literals;

constexpr sorted_array_map lzma_error_desc{
    std::pair{LZMA_NO_CHECK, "input stream has no integrity check"},
    std::pair{LZMA_UNSUPPORTED_CHECK, "cannot calculate the integrity check"},
    std::pair{LZMA_GET_CHECK, "integrity check type is now available"},
    std::pair{LZMA_MEM_ERROR, "cannot allocate memory"},
    std::pair{LZMA_MEMLIMIT_ERROR, "memory usage limit was reached"},
    std::pair{LZMA_FORMAT_ERROR, "file format not recognized"},
    std::pair{LZMA_OPTIONS_ERROR, "invalid or unsupported options"},
    std::pair{LZMA_DATA_ERROR, "data is corrupt"},
    std::pair{LZMA_BUF_ERROR, "no progress is possible"},
    std::pair{LZMA_PROG_ERROR, "programming error"},
    // TODO: re-add when this has arrived in the mainstream...
    // {LZMA_SEEK_NEEDED, "request to change the input file position"},
};

constexpr sorted_array_map kBinaryModes{
    std::pair{"x86"sv, LZMA_FILTER_X86},
    std::pair{"powerpc"sv, LZMA_FILTER_POWERPC},
    std::pair{"ia64"sv, LZMA_FILTER_IA64},
    std::pair{"arm"sv, LZMA_FILTER_ARM},
    std::pair{"armthumb"sv, LZMA_FILTER_ARMTHUMB},
    std::pair{"sparc"sv, LZMA_FILTER_SPARC},
};

constexpr sorted_array_map kCompressionModes{
    std::pair{"fast"sv, LZMA_MODE_FAST},
    std::pair{"normal"sv, LZMA_MODE_NORMAL},
};

constexpr sorted_array_map kMatchFinders{
    std::pair{"hc3"sv, LZMA_MF_HC3}, std::pair{"hc4"sv, LZMA_MF_HC4},
    std::pair{"bt2"sv, LZMA_MF_BT2}, std::pair{"bt3"sv, LZMA_MF_BT3},
    std::pair{"bt4"sv, LZMA_MF_BT4},
};

template <typename T, size_t N>
T find_option(sorted_array_map<std::string_view, T, N> const& options,
              std::string_view name, std::string_view what) {
  if (auto value = options.get(name)) {
    return *value;
  }
  DWARFS_THROW(runtime_error, fmt::format("unknown {} '{}'", what, name));
}

template <typename T, size_t N>
std::string
option_names(sorted_array_map<std::string_view, T, N> const& options) {
  return options | ranges::views::keys | ranges::views::join(", "sv) |
         ranges::to<std::string>;
}

std::string lzma_error_string(lzma_ret err) {
  if (auto it = lzma_error_desc.find(err); it != lzma_error_desc.end()) {
    return it->second;
  }
  return fmt::format("unknown error {}", static_cast<int>(err));
}

class lzma_block_compressor final : public block_compressor::impl {
 public:
  explicit lzma_block_compressor(option_map& om);
  lzma_block_compressor(lzma_block_compressor const& rhs) = default;

  std::unique_ptr<block_compressor::impl> clone() const override {
    return std::make_unique<lzma_block_compressor>(*this);
  }

  std::vector<uint8_t> compress(std::vector<uint8_t> const& data,
                                std::string const* metadata) const override;

  // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
  std::vector<uint8_t> compress(std::vector<uint8_t>&& data,
                                std::string const* metadata) const override {
    return compress(data, metadata);
  }

  compression_type type() const override { return compression_type::LZMA; }

  std::string describe() const override { return description_; }

  std::string metadata_requirements() const override { return {}; }

  compression_constraints
  get_compression_constraints(std::string const&) const override {
    return {};
  }

 private:
  std::vector<uint8_t>
  compress(std::vector<uint8_t> const& data, lzma_filter const* filters) const;

  static uint32_t get_preset(unsigned level, bool extreme) {
    uint32_t preset = level;

    if (extreme) {
      preset |= LZMA_PRESET_EXTREME;
    }

    return preset;
  }

  static lzma_vli get_vli(std::optional<std::string_view> binary) {
    if (!binary) {
      return LZMA_VLI_UNKNOWN;
    }

    return find_option(kBinaryModes, *binary, "binary mode");
  }

  lzma_options_lzma opt_lzma_;
  lzma_vli binary_vli_;
  std::string description_;
};

lzma_block_compressor::lzma_block_compressor(option_map& om) {
  auto level = om.get<unsigned>("level", 9U);
  auto extreme = om.get<bool>("extreme", false);
  auto binary_mode = om.get_optional<std::string>("binary");
  auto dict_size = om.get_optional<unsigned>("dict_size");
  auto mode = om.get_optional<std::string>("mode");
  auto mf = om.get_optional<std::string>("mf");
  auto nice = om.get_optional<unsigned>("nice");
  auto depth = om.get_optional<unsigned>("depth");

  description_ = fmt::format(
      "lzma [level={}{}{}{}{}{}{}{}]", level,
      dict_size ? ", dict_size=" + std::to_string(*dict_size) : "",
      extreme ? ", extreme" : "", binary_mode ? ", binary=" + *binary_mode : "",
      mode ? ", mode=" + *mode : "", mf ? ", mf=" + *mf : "",
      nice ? ", nice=" + std::to_string(*nice) : "",
      depth ? ", depth=" + std::to_string(*depth) : "");

  binary_vli_ = get_vli(binary_mode);

  if (lzma_lzma_preset(&opt_lzma_, get_preset(level, extreme))) {
    DWARFS_THROW(runtime_error, "unsupported preset, possibly a bug");
  }

  if (dict_size) {
    opt_lzma_.dict_size = 1 << *dict_size;
  }

  if (mode) {
    opt_lzma_.mode = find_option(kCompressionModes, *mode, "compression mode");
  }

  if (mf) {
    opt_lzma_.mf = find_option(kMatchFinders, *mf, "match finder");
  }

  if (nice) {
    opt_lzma_.nice_len = *nice;
  }

  if (depth) {
    opt_lzma_.depth = *depth;
  }
}

std::vector<uint8_t>
lzma_block_compressor::compress(std::vector<uint8_t> const& data,
                                lzma_filter const* filters) const {
  lzma_stream s = LZMA_STREAM_INIT;

  if (auto ret = lzma_stream_encoder(&s, filters, LZMA_CHECK_CRC64);
      ret != LZMA_OK) {
    DWARFS_THROW(runtime_error, fmt::format("lzma_stream_encoder: {}",
                                            lzma_error_string(ret)));
  }

  lzma_action action = LZMA_FINISH;

  std::vector<uint8_t> compressed(data.size() - 1);

  s.next_in = data.data();
  s.avail_in = data.size();
  s.next_out = compressed.data();
  s.avail_out = compressed.size();

  lzma_ret ret = lzma_code(&s, action);

  compressed.resize(compressed.size() - s.avail_out);

  lzma_end(&s);

  if (ret == 0) {
    throw bad_compression_ratio_error();
  }

  if (ret == LZMA_STREAM_END) {
    compressed.shrink_to_fit();
  } else {
    DWARFS_THROW(runtime_error, fmt::format("LZMA compression failed: {}",
                                            lzma_error_string(ret)));
  }

  return compressed;
}

std::vector<uint8_t>
lzma_block_compressor::compress(std::vector<uint8_t> const& data,
                                std::string const* /*metadata*/) const {
  auto lzma_opts = opt_lzma_;
  std::array<lzma_filter, 3> filters{{{binary_vli_, nullptr},
                                      {LZMA_FILTER_LZMA2, &lzma_opts},
                                      {LZMA_VLI_UNKNOWN, nullptr}}};

  std::vector<uint8_t> best = compress(data, &filters[1]);

  if (filters[0].id != LZMA_VLI_UNKNOWN) {
    std::vector<uint8_t> compressed = compress(data, filters.data());

    if (compressed.size() < best.size()) {
      best.swap(compressed);
    }
  }

  return best;
}

class lzma_block_decompressor final : public block_decompressor::impl {
 public:
  lzma_block_decompressor(uint8_t const* data, size_t size,
                          std::vector<uint8_t>& target)
      : stream_(LZMA_STREAM_INIT)
      , decompressed_(target)
      , uncompressed_size_(get_uncompressed_size(data, size)) {
    stream_.next_in = data;
    stream_.avail_in = size;
    if (auto ret = lzma_stream_decoder(&stream_, UINT64_MAX, LZMA_CONCATENATED);
        ret != LZMA_OK) {
      DWARFS_THROW(runtime_error, fmt::format("lzma_stream_decoder: {}",
                                              lzma_error_string(ret)));
    }
    try {
      decompressed_.reserve(uncompressed_size_);
    } catch (std::bad_alloc const&) {
      DWARFS_THROW(
          runtime_error,
          fmt::format("could not reserve {} bytes for decompressed block",
                      uncompressed_size_));
    }
  }

  ~lzma_block_decompressor() override { lzma_end(&stream_); }

  compression_type type() const override { return compression_type::LZMA; }

  std::optional<std::string> metadata() const override { return std::nullopt; }

  bool decompress_frame(size_t frame_size) override {
    if (!error_.empty()) {
      DWARFS_THROW(runtime_error, error_);
    }

    lzma_action action = LZMA_RUN;

    if (decompressed_.size() + frame_size > uncompressed_size_) {
      frame_size = uncompressed_size_ - decompressed_.size();
      action = LZMA_FINISH;
    }

    assert(frame_size > 0);

    size_t offset = decompressed_.size();
    decompressed_.resize(offset + frame_size);

    stream_.next_out = decompressed_.data() + offset;
    stream_.avail_out = frame_size;

    lzma_ret ret = lzma_code(&stream_, action);

    if (ret == LZMA_STREAM_END) {
      lzma_end(&stream_);
    }

    if (ret != (action == LZMA_RUN ? LZMA_OK : LZMA_STREAM_END) ||
        stream_.avail_out != 0) {
      decompressed_.clear();
      error_ =
          fmt::format("LZMA decompression failed: {}", lzma_error_string(ret));
      DWARFS_THROW(runtime_error, error_);
    }

    return ret == LZMA_STREAM_END;
  }

  size_t uncompressed_size() const override { return uncompressed_size_; }

 private:
  static size_t get_uncompressed_size(uint8_t const* data, size_t size);

  lzma_stream stream_;
  std::vector<uint8_t>& decompressed_;
  size_t const uncompressed_size_;
  std::string error_;
};

size_t lzma_block_decompressor::get_uncompressed_size(uint8_t const* data,
                                                      size_t size) {
  if (size < 2 * LZMA_STREAM_HEADER_SIZE) {
    DWARFS_THROW(runtime_error, "lzma compressed block is too small");
  }

  lzma_stream s = LZMA_STREAM_INIT;
  file_off_t pos = size - LZMA_STREAM_HEADER_SIZE;
  uint32_t const* ptr = reinterpret_cast<uint32_t const*>(data + size) - 1;

  while (*ptr == 0) {
    pos -= 4;
    --ptr;

    if (pos < 2 * LZMA_STREAM_HEADER_SIZE) {
      DWARFS_THROW(runtime_error, "data error (stream padding)");
    }
  }

  lzma_stream_flags footer_flags;

  if (auto ret = lzma_stream_footer_decode(&footer_flags, data + pos);
      ret != LZMA_OK) {
    DWARFS_THROW(runtime_error, fmt::format("lzma_stream_footer_decode: {}",
                                            lzma_error_string(ret)));
  }

  lzma_vli index_size = footer_flags.backward_size;
  if (static_cast<lzma_vli>(pos) < index_size + LZMA_STREAM_HEADER_SIZE) {
    DWARFS_THROW(runtime_error, "data error (index size)");
  }

  pos -= index_size;
  lzma_index* index = nullptr;

  if (auto ret = lzma_index_decoder(&s, &index, UINT64_MAX); ret != LZMA_OK) {
    DWARFS_THROW(runtime_error,
                 fmt::format("lzma_index_decoder: {}", lzma_error_string(ret)));
  }

  s.avail_in = index_size;
  s.next_in = data + pos;

  lzma_ret ret = lzma_code(&s, LZMA_RUN);
  if (ret != LZMA_STREAM_END || s.avail_in != 0) {
    DWARFS_THROW(runtime_error,
                 fmt::format("lzma_code(): {} (avail_in={})",
                             lzma_error_string(ret), s.avail_in));
  }

  pos -= LZMA_STREAM_HEADER_SIZE;
  if (static_cast<lzma_vli>(pos) < lzma_index_total_size(index)) {
    DWARFS_THROW(runtime_error, "data error (index total size)");
  }

  size_t usize = lzma_index_uncompressed_size(index);

  // TODO: wrap this in some RAII container, as error handling is horrible...
  lzma_end(&s);
  lzma_index_end(index, nullptr);

  return usize;
}

class lzma_compression_factory : public compression_factory {
 public:
  static constexpr compression_type type{compression_type::LZMA};

  std::string_view name() const override { return "lzma"; }

  std::string_view description() const override {
    static std::string const s_desc{
        fmt::format("LZMA compression (liblzma {})", ::lzma_version_string())};
    return s_desc;
  }

  std::vector<std::string> const& options() const override { return options_; }

  std::set<std::string> library_dependencies() const override {
    return {fmt::format("liblzma-{}", ::lzma_version_string())};
  }

  std::unique_ptr<block_compressor::impl>
  make_compressor(option_map& om) const override {
    return std::make_unique<lzma_block_compressor>(om);
  }

  std::unique_ptr<block_decompressor::impl>
  make_decompressor(std::span<uint8_t const> data,
                    std::vector<uint8_t>& target) const override {
    return std::make_unique<lzma_block_decompressor>(data.data(), data.size(),
                                                     target);
  }

 private:
  std::vector<std::string> const options_{
      "level=[0..9]",
      "dict_size=[12..30]",
      "extreme",
      "binary={" + option_names(kBinaryModes) + "}",
      "mode={" + option_names(kCompressionModes) + "}",
      "mf={" + option_names(kMatchFinders) + "}",
      "nice=[0..273]",
      "depth=[0..4294967295]",
  };
};

} // namespace

REGISTER_COMPRESSION_FACTORY(lzma_compression_factory)

} // namespace dwarfs
