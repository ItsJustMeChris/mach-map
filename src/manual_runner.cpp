#include "complex_manual_api.hpp"
#include "manual_mapper.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits.h>
#include <mach-o/dyld.h>
#include <netdb.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

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
    if (path.empty()) {
        return false;
    }

    const std::string canon = canonicalPath(path);
    for (const auto &img : images) {
        if (img == canon) {
            return true;
        }
    }
    return false;
}

std::string executableDirectory() {
    uint32_t size = PATH_MAX;
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) == -1) {
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
            return ".";
        }
    }

    char resolved[PATH_MAX];
    const char *path = realpath(buffer.data(), resolved);
    std::string fullPath = path ? std::string(path) : std::string(buffer.data());
    const std::size_t pos = fullPath.find_last_of('/');
    if (pos == std::string::npos) {
        return ".";
    }
    return fullPath.substr(0, pos);
}

bool isHttpUrl(const std::string &path) {
    constexpr std::string_view prefix = "http://";
    return path.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), path.begin());
}

std::vector<uint8_t> downloadHttp(const std::string &url) {
    constexpr std::string_view prefix = "http://";
    if (!isHttpUrl(url)) {
        throw std::runtime_error("Only http:// URLs are supported");
    }

    const std::size_t hostStart = prefix.size();
    const std::size_t slashPos = url.find('/', hostStart);
    std::string hostPort = slashPos == std::string::npos ? url.substr(hostStart)
                                                         : url.substr(hostStart, slashPos - hostStart);
    std::string path = slashPos == std::string::npos ? "/" : url.substr(slashPos);

    std::string host;
    std::string port = "80";
    const std::size_t colonPos = hostPort.find(':');
    if (colonPos != std::string::npos) {
        host = hostPort.substr(0, colonPos);
        port = hostPort.substr(colonPos + 1);
    } else {
        host = hostPort;
    }

    if (host.empty()) {
        throw std::runtime_error("Invalid HTTP URL (missing host): " + url);
    }

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = nullptr;
    int gai = getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
    if (gai != 0) {
        throw std::runtime_error("getaddrinfo failed: " + std::string(gai_strerror(gai)));
    }

    int sockfd = -1;
    for (struct addrinfo *rp = result; rp != nullptr; rp = rp->ai_next) {
        sockfd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd == -1) {
            continue;
        }
        if (::connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        ::close(sockfd);
        sockfd = -1;
    }

    if (sockfd == -1) {
        freeaddrinfo(result);
        throw std::runtime_error("Failed to connect to " + host + ":" + port);
    }

    std::ostringstream request;
    request << "GET " << path << " HTTP/1.0\r\n"
            << "Host: " << host << "\r\n"
            << "Connection: close\r\n"
            << "\r\n";

    const std::string requestStr = request.str();
    ssize_t sent = ::send(sockfd, requestStr.data(), requestStr.size(), 0);
    if (sent < 0 || static_cast<size_t>(sent) != requestStr.size()) {
        ::close(sockfd);
        freeaddrinfo(result);
        throw std::runtime_error("Failed to send HTTP request");
    }

    std::vector<uint8_t> buffer;
    std::array<uint8_t, 4096> chunk{};
    ssize_t received = 0;
    while ((received = ::recv(sockfd, chunk.data(), chunk.size(), 0)) > 0) {
        buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + received);
    }
    ::close(sockfd);
    freeaddrinfo(result);

    if (buffer.empty()) {
        throw std::runtime_error("Empty HTTP response from " + url);
    }

    const std::string delimiter = "\r\n\r\n";
    auto headerEnd = std::search(buffer.begin(), buffer.end(), delimiter.begin(), delimiter.end());
    if (headerEnd == buffer.end()) {
        throw std::runtime_error("Malformed HTTP response (missing headers)");
    }

    const size_t headerLen =
        static_cast<size_t>(std::distance(buffer.begin(), headerEnd)) + delimiter.size();
    std::string header(buffer.begin(), buffer.begin() + headerLen);

    std::istringstream headerStream(header);
    std::string statusLine;
    if (!std::getline(headerStream, statusLine)) {
        throw std::runtime_error("Malformed HTTP status line");
    }
    if (statusLine.find("200") == std::string::npos) {
        throw std::runtime_error("Unexpected HTTP status: " + statusLine);
    }

    std::vector<uint8_t> body(buffer.begin() + headerLen, buffer.end());
    if (body.empty()) {
        throw std::runtime_error("HTTP response body is empty");
    }

    return body;
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
        const std::string baseDir = executableDirectory();
        targets.emplace_back(baseDir + "/libmanual.dylib");
        targets.emplace_back(baseDir + "/libcomplex_manual.dylib");
        targets.emplace_back("http://localhost:3000/libmanual.dylib");
    }

    reportDyldImages("dyld images before manual loads");
    std::cout << "\n";

    bool anySuccess = false;
    bool anyFailure = false;

    for (const std::string &inputPath : targets) {
        const bool remote = isHttpUrl(inputPath);
        std::cout << "== Loading " << inputPath << "\n";
        try {
            LoadedImage image;
            std::vector<uint8_t> remoteBuffer;

            if (remote) {
                remoteBuffer = downloadHttp(inputPath);
                std::cout << "[http] downloaded " << remoteBuffer.size() << " bytes\n";
                image = loadMachOImageFromBuffer(remoteBuffer.data(), remoteBuffer.size());
            } else {
                image = loadMachOImage(inputPath);
            }

            if (!image.valid()) {
                std::cerr << "Failed to map image: " << inputPath << "\n";
                anyFailure = true;
                continue;
            }

            std::cout << "Loaded image at 0x" << std::hex << image.baseAddress() << std::dec << "\n";

            runSimpleDemo(image);
            runComplexDemo(image);

            auto afterImages = currentDyldImages();
            bool registered = remote ? false : dyldContainsImage(afterImages, inputPath);
            std::cout << "[dyld] image present: " << (registered ? "yes" : "no")
                      << (remote ? " (remote load)" : "") << "\n";
            reportDyldImages("dyld images after loading " + inputPath);

            anySuccess = true;
        } catch (const std::exception &ex) {
            std::cerr << "Error while loading " << inputPath << ": " << ex.what() << "\n";
            anyFailure = true;
        } catch (...) {
            std::cerr << "Unknown error occurred while loading " << inputPath << "\n";
            anyFailure = true;
        }

        std::cout << "\n";
    }

    if (!anySuccess) {
        return EXIT_FAILURE;
    }
    return anyFailure ? EXIT_FAILURE : EXIT_SUCCESS;
}
