#include "complex_manual_api.hpp"

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>

namespace {

struct RunningStats {
    double sum;
    double sumSquares;
    std::uint64_t count;
    double minimum;
    double maximum;
};

RunningStats g_stats = {0.0, 0.0, 0, 1.7e308, -1.7e308};

double g_recentValues[64] = {0.0};
std::size_t g_recentCount = 0;
std::size_t g_recentIndex = 0;

std::uint32_t g_histogram[kComplexHistogramBins] = {0};
const double g_histogramEdges[kComplexHistogramBins + 1] = {
    -100.0, -50.0, -25.0, -10.0, -5.0, -1.0, -0.25, 0.0,
    0.25,   1.0,   5.0,   10.0,  25.0,  50.0,  100.0, 250.0, 1e9};

double g_trigAccum = 0.0;
std::size_t g_nameLength = 0;
char g_banner[] = "manual-stats-dylib";

std::size_t locateHistogramBin(double value) {
    for (std::size_t i = 0; i < kComplexHistogramBins; ++i) {
        if (value >= g_histogramEdges[i] && value < g_histogramEdges[i + 1]) {
            return i;
        }
    }
    return kComplexHistogramBins - 1;
}

double clampPercentile(double value) {
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 100.0) {
        return 100.0;
    }
    return value;
}

void copyRecentValues(double *dest, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        dest[i] = g_recentValues[i];
    }
}

void sortValues(double *values, std::size_t count) {
    for (std::size_t i = 1; i < count; ++i) {
        const double key = values[i];
        std::size_t j = i;
        while (j > 0 && values[j - 1] > key) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = key;
    }
}

double computePercentile(double percentile) {
    if (g_recentCount == 0) {
        return 0.0;
    }

    double buffer[64];
    copyRecentValues(buffer, g_recentCount);
    sortValues(buffer, g_recentCount);

    const double scaled =
        (clampPercentile(percentile) / 100.0) * static_cast<double>(g_recentCount - 1);
    const std::size_t lowerIndex = static_cast<std::size_t>(scaled);
    const double fraction = scaled - static_cast<double>(lowerIndex);

    if (lowerIndex >= g_recentCount - 1) {
        return buffer[g_recentCount - 1];
    }

    const double lower = buffer[lowerIndex];
    const double upper = buffer[lowerIndex + 1];
    return lower + (upper - lower) * fraction;
}

} // namespace

extern "C" void stats_reset() {
    g_stats.sum = 0.0;
    g_stats.sumSquares = 0.0;
    g_stats.count = 0;
    g_stats.minimum = 1.7e308;
    g_stats.maximum = -1.7e308;

    for (std::size_t i = 0; i < kComplexHistogramBins; ++i) {
        g_histogram[i] = 0;
    }

    for (std::size_t i = 0; i < 64; ++i) {
        g_recentValues[i] = 0.0;
    }
    g_recentCount = 0;
    g_recentIndex = 0;

    g_trigAccum = 0.0;
    g_nameLength = std::strlen(g_banner);
}

extern "C" void stats_add(double value) {
    if (value < g_stats.minimum) {
        g_stats.minimum = value;
    }
    if (value > g_stats.maximum) {
        g_stats.maximum = value;
    }

    g_stats.sum += value;
    g_stats.sumSquares += std::pow(value, 2.0);
    g_stats.count += 1;

    const double harmonic = std::sin(value * 0.1) + std::cos(value * 0.05);
    const double adjusted = std::fmod(value, 17.0) + harmonic;
    g_trigAccum += adjusted;

    const std::size_t bin = locateHistogramBin(value);
    if (bin < kComplexHistogramBins) {
        g_histogram[bin] += 1;
    }

    g_recentValues[g_recentIndex] = value;
    g_recentIndex = (g_recentIndex + 1) % 64;
    if (g_recentCount < 64) {
        g_recentCount += 1;
    }
}

extern "C" void stats_get_summary(ComplexStatsSummary *outSummary) {
    if (!outSummary) {
        return;
    }

    (void)g_nameLength;

    ComplexStatsSummary summary{};
    summary.count = g_stats.count;

    if (g_stats.count == 0) {
        summary.mean = 0.0;
        summary.variance = 0.0;
        summary.standardDeviation = 0.0;
        summary.minimum = 0.0;
        summary.maximum = 0.0;
    } else {
        const double countAsDouble = static_cast<double>(g_stats.count);
        summary.mean = g_stats.sum / countAsDouble;

        const double meanSquare = summary.mean * summary.mean;
        const double averageSquares = g_stats.sumSquares / countAsDouble;
        double variance = averageSquares - meanSquare;
        if (variance < 0.0) {
            variance = 0.0;
        }
        summary.variance = variance;
        summary.standardDeviation = __builtin_sqrt(variance);
        summary.minimum = g_stats.minimum;
        summary.maximum = g_stats.maximum;
    }

    *outSummary = summary;
}

extern "C" void stats_get_histogram(std::uint32_t *outBins, std::size_t length) {
    if (!outBins || length == 0) {
        return;
    }

    const std::size_t binsToCopy =
        length < kComplexHistogramBins ? length : kComplexHistogramBins;
    for (std::size_t i = 0; i < binsToCopy; ++i) {
        outBins[i] = g_histogram[i];
    }

    for (std::size_t i = binsToCopy; i < length; ++i) {
        outBins[i] = 0;
    }
}

extern "C" double stats_compute_percentile(double percentile) {
    return computePercentile(percentile);
}

extern "C" std::size_t stats_recent_count() {
    return g_recentCount;
}
