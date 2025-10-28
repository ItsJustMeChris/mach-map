#include <cstdint>

namespace {

constexpr int kMultiplier = 3;
constexpr int kOffset = 7;

const char kMessage[] = "Manual mapping success!";

} // namespace

extern "C" int manual_entry(int value) {
    // Simple arithmetic to avoid external dependencies.
    return value * kMultiplier + kOffset;
}

extern "C" const char *manual_message() {
    return kMessage;
}
