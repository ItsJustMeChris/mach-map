#include "complex_manual_api.hpp"
#include "manual_mapper.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mach-o/dyld.h>
#include <string>
#include <vector>
#include <limits.h>
#include <unistd.h>

namespace {

std::string canonicalPath(const std::string &path) {
    char buffer[PATH_MAX];
    if (realpath(path.c_str(), buffer)) {
        return std::string(buffer);
    }
    return path;
}

std::vector<std::string> currentDyldImages() {
    std::vector<std::string> images;
    const uint32_t count = _dyld_image_count();
    images.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const char *name = _dyld_get_image_name(i);
        if (name && *name) {
            images.emplace_back(name);
        }
    }
    return images;
}

void reportDyldImages(const std::string &label) {
    auto images = currentDyldImages();
    std::cout << label << " (" << images.size() << " images)\n";
    for (const auto &img : images) {
        std::cout << "  " << img << "\n";
    }
}

bool dyldContainsImage(const std::vector<std::string> &images, const std::string &path) {
    const std::string canon = canonicalPath(path);
    for (const auto &img : images) {
        if (img == canon) {
            return true;
        }
    }
    return false;
}

void runSimpleDemo(const LoadedImage &image) {
    using EntryFn = int (*)(int);
    using MessageFn = const char *(*)();

    EntryFn entry = image.getSymbol<EntryFn>("manual_entry");
    if (!entry) {
        std::cout << "manual_entry() not available; skipping simple demo\n";
        return;
    }

    MessageFn messageFn = image.getSymbol<MessageFn>("manual_message");

    const int input = 5;
    const int result = entry(input);

    std::cout << "[simple] manual_entry(" << input << ") = " << result << "\n";
    if (messageFn) {
        std::cout << "[simple] manual_message(): " << messageFn() << "\n";
    } else {
        std::cout << "[simple] manual_message() not found\n";
    }
}

void runComplexDemo(const LoadedImage &image) {
    using StatsResetFn = void (*)();
    using StatsAddFn = void (*)(double);
    using StatsSummaryFn = void (*)(ComplexStatsSummary *);
    using StatsHistogramFn = void (*)(std::uint32_t *, std::size_t);
    using StatsPercentileFn = double (*)(double);
    using StatsRecentCountFn = std::size_t (*)();

    StatsResetFn statsReset = image.getSymbol<StatsResetFn>("stats_reset");
    StatsAddFn statsAdd = image.getSymbol<StatsAddFn>("stats_add");
    StatsSummaryFn statsGetSummary = image.getSymbol<StatsSummaryFn>("stats_get_summary");
    StatsHistogramFn statsGetHistogram = image.getSymbol<StatsHistogramFn>("stats_get_histogram");
    StatsPercentileFn statsComputePercentile =
        image.getSymbol<StatsPercentileFn>("stats_compute_percentile");
    StatsRecentCountFn statsRecentCount =
        image.getSymbol<StatsRecentCountFn>("stats_recent_count");

    if (!statsReset || !statsAdd || !statsGetSummary || !statsGetHistogram ||
        !statsComputePercentile || !statsRecentCount) {
        std::cout << "[complex] Required stats_* exports not found; skipping complex demo\n";
        return;
    }

    statsReset();

    const double samples[] = {42.0, 12.5, -7.0, 98.0, 0.5, 0.25, 15.0,
                              -2.0, 77.0, -40.0, 1.5, 200.0, 3.3, -0.1};

    for (double value : samples) {
        statsAdd(value);
    }

    ComplexStatsSummary summary{};
    statsGetSummary(&summary);

    std::cout << "[complex] count=" << summary.count << " mean=" << summary.mean
              << " variance=" << summary.variance << " stddev=" << summary.standardDeviation
              << " min=" << summary.minimum << " max=" << summary.maximum << "\n";

    std::uint32_t histogram[kComplexHistogramBins];
    statsGetHistogram(histogram, kComplexHistogramBins);

    std::cout << "[complex] histogram:";
    for (std::size_t i = 0; i < kComplexHistogramBins; ++i) {
        std::cout << " " << histogram[i];
    }
    std::cout << "\n";

    const double p90 = statsComputePercentile(90.0);
    const double p10 = statsComputePercentile(10.0);
    const std::size_t recent = statsRecentCount();

    std::cout << "[complex] percentile10=" << p10 << " percentile90=" << p90
              << " recentCount=" << recent << "\n";
}

} // namespace

int main(int argc, char *argv[]) {
    std::vector<std::string> targets;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            targets.emplace_back(argv[i]);
        }
    } else {
        targets.emplace_back("./libmanual.dylib");
        targets.emplace_back("./libcomplex_manual.dylib");
    }

    reportDyldImages("dyld images before manual loads");
    std::cout << "\n";

    bool anySuccess = false;
    bool anyFailure = false;

    for (const std::string &dylibPath : targets) {
        std::cout << "== Loading " << dylibPath << "\n";
        try {
            LoadedImage image = loadMachOImage(dylibPath);
            if (!image.valid()) {
                std::cerr << "Failed to map image: " << dylibPath << "\n";
                anyFailure = true;
                continue;
            }

            std::cout << "Loaded image at 0x" << std::hex << image.baseAddress() << std::dec << "\n";

            runSimpleDemo(image);
            runComplexDemo(image);

            auto afterImages = currentDyldImages();
            bool registered = dyldContainsImage(afterImages, dylibPath);
            std::cout << "[dyld] image present: " << (registered ? "yes" : "no") << "\n";
            reportDyldImages("dyld images after loading " + dylibPath);

            anySuccess = true;
        } catch (const std::exception &ex) {
            std::cerr << "Error while loading " << dylibPath << ": " << ex.what() << "\n";
            anyFailure = true;
        } catch (...) {
            std::cerr << "Unknown error occurred while loading " << dylibPath << "\n";
            anyFailure = true;
        }

        std::cout << "\n";
    }

    if (!anySuccess) {
        return EXIT_FAILURE;
    }
    return anyFailure ? EXIT_FAILURE : EXIT_SUCCESS;
}
