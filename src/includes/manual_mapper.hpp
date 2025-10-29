#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

using SymbolResolver = std::function<void *(const std::string &)>;

struct LoaderOptions {
    SymbolResolver symbolResolver;
    bool verbose = false;
};

class LoadedImage {
public:
    LoadedImage() = default;
    ~LoadedImage();

    LoadedImage(const LoadedImage &) = delete;
    LoadedImage &operator=(const LoadedImage &) = delete;

    LoadedImage(LoadedImage &&other) noexcept;
    LoadedImage &operator=(LoadedImage &&other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return base_ != nullptr; }
    [[nodiscard]] uintptr_t baseAddress() const noexcept {
        return reinterpret_cast<uintptr_t>(base_);
    }
    [[nodiscard]] ptrdiff_t slide() const noexcept { return slide_; }

    void *findSymbol(const std::string &name) const;

    template <typename T> T getSymbol(const std::string &name) const {
        return reinterpret_cast<T>(findSymbol(name));
    }

    LoadedImage(uint8_t *base, size_t size, ptrdiff_t slide,
                std::unordered_map<std::string, void *> symbols);

private:
    friend LoadedImage loadMachOImage(const std::string &path);
    friend LoadedImage loadMachOImageFromBuffer(const uint8_t *data, size_t size);
    friend void unloadMachOImage(LoadedImage &image);

    uint8_t *base_ = nullptr;
    size_t size_ = 0;
    ptrdiff_t slide_ = 0;
    std::unordered_map<std::string, void *> symbols_;
};

LoadedImage loadMachOImage(const std::string &path);
LoadedImage loadMachOImage(const std::string &path, const LoaderOptions &options);
LoadedImage loadMachOImageFromBuffer(const uint8_t *data, size_t size);
LoadedImage loadMachOImageFromBuffer(const uint8_t *data, size_t size, const LoaderOptions &options);
void unloadMachOImage(LoadedImage &image);
