#pragma once

#include <cstddef>
#include <cstdint>

struct ComplexStatsSummary {
    double mean;
    double variance;
    double minimum;
    double maximum;
    std::uint64_t count;
    double standardDeviation;
};

inline constexpr std::size_t kComplexHistogramBins = 16;
