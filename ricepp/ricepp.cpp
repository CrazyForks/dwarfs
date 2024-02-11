/* vim:set ts=2 sw=2 sts=2 et: */
/**
 * \author     Marcus Holland-Moritz (github@mhxnet.de)
 * \copyright  Copyright (c) Marcus Holland-Moritz
 *
 * This file is part of ricepp.
 *
 * ricepp is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ricepp is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ricepp.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include <ricepp/ricepp.h>

#include "ricepp_cpuspecific.h"

namespace ricepp {

namespace {

detail::cpu_variant get_cpu_variant_init() {
#ifndef _WIN32
#if defined(__has_builtin)
#if __has_builtin(__builtin_cpu_supports)
  __builtin_cpu_init();

  bool const has_avx512vl = __builtin_cpu_supports("avx512vl");
  bool const has_avx512vbmi = __builtin_cpu_supports("avx512vbmi");
  bool const has_bmi2 = __builtin_cpu_supports("bmi2");

  if (has_avx512vl && has_avx512vbmi && has_bmi2) {
    return detail::cpu_variant::has_bmi2_avx512;
  }

  if (has_bmi2) {
    return detail::cpu_variant::has_bmi2;
  }
#endif
#endif
#endif

  return detail::cpu_variant::fallback;
}

detail::cpu_variant get_cpu_variant() {
  static detail::cpu_variant const variant = get_cpu_variant_init();
  return variant;
}

void show_cpu_variant(std::string_view variant) {
  if (std::getenv("RICEPP_SHOW_CPU_VARIANT")) {
    std::cerr << "ricepp: using " << variant << " CPU variant\n";
  }
}

void show_cpu_variant_once(std::string_view variant) {
  static auto const _ = [&variant]() {
    show_cpu_variant(variant);
    return true;
  }();
}

} // namespace

template <>
std::unique_ptr<codec_interface<uint16_t>>
create_codec<uint16_t>(codec_config const& config) {
  switch (get_cpu_variant()) {
#ifdef RICEPP_CPU_BMI2_AVX512
  case detail::cpu_variant::has_bmi2_avx512:
    show_cpu_variant_once("BMI2+AVX512");
    return detail::create_codec_cpuspecific_<
        uint16_t, detail::cpu_variant::has_bmi2_avx512>(config);
#endif

#ifdef RICEPP_CPU_BMI2
  case detail::cpu_variant::has_bmi2:
    show_cpu_variant_once("BMI2");
    return detail::create_codec_cpuspecific_<uint16_t,
                                             detail::cpu_variant::has_bmi2>(
        config);
#endif

  default:
    show_cpu_variant_once("fallback");
    return detail::create_codec_cpuspecific_<uint16_t,
                                             detail::cpu_variant::fallback>(
        config);
  }

  throw std::runtime_error("internal error: unknown CPU variant");
}

} // namespace ricepp
