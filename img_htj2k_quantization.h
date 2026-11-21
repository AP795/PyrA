#pragma once

// Qfactor recipe and visual-weight data adapted from OpenJPH 0.33.0,
// src/core/codestream/ojph_params.cpp, under the following license:
//
// BSD 2-Clause License
//
// Copyright (c) 2019, Aous Naman
// Copyright (c) 2019, Kakadu Software Pty Ltd, Australia
// Copyright (c) 2019, The University of New South Wales, Australia
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <array>
#include <cmath>

namespace priml::detail {

inline constexpr unsigned kHtj2kMaxDecompositions = 5;
inline constexpr auto kHtj2kQuantizationWeights = 3 * kHtj2kMaxDecompositions + 1;

struct Htj2kQuantization {
	float delta = 0;
	std::array<float, kHtj2kQuantizationWeights> weights{};
};


inline Htj2kQuantization htj2k_range_quantization ( int quality, int bits,
	int maximum, unsigned component, unsigned decompositions )
{
// The public explicit-quantization API supplies the wavelet energy gains.
// Preserve the Qfactor visual weights and component gains; only the reference
// signal range changes. The rounding floor stays in actual stored-code units.
	constexpr auto low = 65.0f;
	constexpr auto high = 97.0f;
	constexpr auto alpha_low = 0.04f;
	constexpr auto alpha_high = 0.10f;
	const auto q = static_cast<float>( quality );
	const auto multiplier = q < 50.0f ? 50.0f / q : 2.0f * (1.0f - q / 100.0f);
	auto power = 1.0f;
	auto alpha = alpha_low;
	if (q >= high) {
		power = 0.0f;
		alpha = alpha_high;
	} else if (q > low) {
		constexpr auto low_multiplier = 2.0f * (1.0f - low / 100.0f);
		constexpr auto high_multiplier = 2.0f * (1.0f - high / 100.0f);
		power = std::log( multiplier / high_multiplier ) /
			std::log( low_multiplier / high_multiplier );
		alpha = alpha_high * std::pow( alpha_low / alpha_high, power );
	}
	constexpr auto visual = std::array{
		std::array{ 0.0901f, 0.2758f, 0.2758f, 0.7018f, 0.8378f, 0.8378f,
			1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
		std::array{ 0.0263f, 0.0863f, 0.0863f, 0.1362f, 0.2564f, 0.2564f,
			0.3346f, 0.4691f, 0.4691f, 0.5444f, 0.6523f, 0.6523f, 0.7078f, 0.7797f, 0.7797f },
		std::array{ 0.0773f, 0.1835f, 0.1835f, 0.2598f, 0.4130f, 0.4130f,
			0.5040f, 0.6464f, 0.6464f, 0.7220f, 0.8254f, 0.8254f, 0.8769f, 0.9424f, 0.9424f }
	};
	constexpr auto component_gain = std::array{ 1.0f, 1.8051f / 1.7321f, 1.5734f / 1.7321f };
	auto result = Htj2kQuantization{};
	const auto stored_unit = std::ldexp( 1.0f, -bits );
	result.delta = (alpha * multiplier * static_cast<float>( maximum + 1 ) +
		std::sqrt( 0.5f )) * stored_unit / component_gain[component];
	for (auto i = 0u; i < 3 * decompositions; ++i)
		result.weights[i] = std::pow( visual[component][i], power );
	result.weights[3 * decompositions] = 1.0f;
	return result;
}

}
