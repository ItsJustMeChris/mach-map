#include "manual_mapper.hpp"

#include <dlfcn.h>
#include <libkern/OSByteOrder.h>
#include <mach-o/dyld.h>
#include <mach-o/fat.h>
#include <mach-o/fixup-chains.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct SegmentRuntime {
    const segment_command_64 *command = nullptr;
    uint8_t *mapped = nullptr;
};

std::vector<uint8_t> readFile(const std::string &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Failed to open file: " + path);
    }

    const std::streamsize size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error("File is empty: " + path);
    }

    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char *>(buffer.data()), size);
    if (!file) {
        throw std::runtime_error("Failed to read entire file: " + path);
    }

    return buffer;
}

void *defaultSymbolResolver(const std::string &name) {
    const char *raw = name.c_str();
    void *resolved = dlsym(RTLD_DEFAULT, raw);
    if (!resolved && !name.empty() && name.front() == '_') {
        resolved = dlsym(RTLD_DEFAULT, raw + 1);
    }
    return resolved;
}

SymbolResolver selectResolver(const LoaderOptions &options) {
    if (options.symbolResolver) {
        return options.symbolResolver;
    }
    return SymbolResolver(defaultSymbolResolver);
}

uint64_t readULEB128(const uint8_t *&cursor, const uint8_t *end) {
    uint64_t result = 0;
    int bit = 0;
    while (cursor < end) {
        const uint8_t byte = *cursor++;
        result |= static_cast<uint64_t>(byte & 0x7f) << bit;
        if ((byte & 0x80) == 0) {
            return result;
        }
        bit += 7;
        if (bit > 63) {
            throw std::runtime_error("ULEB128 value too large");
        }
    }
    throw std::runtime_error("Malformed ULEB128 (buffer overrun)");
}

int64_t readSLEB128(const uint8_t *&cursor, const uint8_t *end) {
    int64_t result = 0;
    int bit = 0;
    uint8_t byte = 0;

    while (cursor < end) {
        byte = *cursor++;
        result |= static_cast<int64_t>(byte & 0x7f) << bit;
        bit += 7;
        if ((byte & 0x80) == 0) {
            break;
        }
    }

    if (cursor > end) {
        throw std::runtime_error("Malformed SLEB128 (buffer overrun)");
    }

    if ((bit < 64) && (byte & 0x40)) {
        result |= -1LL << bit;
    }

    return result;
}

uint8_t *segmentAddress(std::vector<SegmentRuntime> &segments, uint8_t index,
                        uint64_t offset, size_t pointerSize) {
    if (index >= segments.size()) {
        throw std::runtime_error("Bind/rebase references out-of-range segment index");
    }

    SegmentRuntime &segment = segments[index];
    if (!segment.command || !segment.mapped) {
        throw std::runtime_error("Bind/rebase references unmapped segment");
    }
    if (offset + pointerSize > segment.command->vmsize) {
        throw std::runtime_error("Bind/rebase offset beyond segment bounds");
    }
    return segment.mapped + offset;
}

void applyRebaseOpcodes(std::vector<SegmentRuntime> &segments, const uint8_t *opcodes,
                        size_t size, ptrdiff_t slide) {
    if (size == 0) {
        return;
    }

    const uint8_t *cursor = opcodes;
    const uint8_t *end = opcodes + size;
    constexpr size_t pointerSize = sizeof(uint64_t);

    uint8_t segmentIndex = 0;
    uint64_t segmentOffset = 0;
    uint8_t type = REBASE_TYPE_POINTER;

    while (cursor < end) {
        const uint8_t byte = *cursor++;
        const uint8_t opcode = byte & REBASE_OPCODE_MASK;
        const uint8_t immediate = byte & REBASE_IMMEDIATE_MASK;

        switch (opcode) {
        case REBASE_OPCODE_DONE:
            return;
        case REBASE_OPCODE_SET_TYPE_IMM:
            type = immediate;
            break;
        case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
            segmentIndex = immediate;
            segmentOffset = readULEB128(cursor, end);
            break;
        case REBASE_OPCODE_ADD_ADDR_ULEB:
            segmentOffset += readULEB128(cursor, end);
            break;
        case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
            segmentOffset += static_cast<uint64_t>(immediate) * pointerSize;
            break;
        case REBASE_OPCODE_DO_REBASE_IMM_TIMES: {
            if (type != REBASE_TYPE_POINTER) {
                throw std::runtime_error("Unsupported rebase type");
            }
            for (uint8_t i = 0; i < immediate; ++i) {
                uint8_t *location =
                    segmentAddress(segments, segmentIndex, segmentOffset, pointerSize);
                auto *value = reinterpret_cast<uint64_t *>(location);
                *value += static_cast<int64_t>(slide);
                segmentOffset += pointerSize;
            }
            break;
        }
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES: {
            if (type != REBASE_TYPE_POINTER) {
                throw std::runtime_error("Unsupported rebase type");
            }
            const uint64_t count = readULEB128(cursor, end);
            for (uint64_t i = 0; i < count; ++i) {
                uint8_t *location =
                    segmentAddress(segments, segmentIndex, segmentOffset, pointerSize);
                auto *value = reinterpret_cast<uint64_t *>(location);
                *value += static_cast<int64_t>(slide);
                segmentOffset += pointerSize;
            }
            break;
        }
        case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB: {
            if (type != REBASE_TYPE_POINTER) {
                throw std::runtime_error("Unsupported rebase type");
            }
            uint8_t *location =
                segmentAddress(segments, segmentIndex, segmentOffset, pointerSize);
            auto *value = reinterpret_cast<uint64_t *>(location);
            *value += static_cast<int64_t>(slide);
            segmentOffset += readULEB128(cursor, end) + pointerSize;
            break;
        }
        default:
            throw std::runtime_error("Unknown rebase opcode");
        }
    }
}

struct BindContext {
    std::vector<SegmentRuntime> &segments;
    SymbolResolver resolver;
    bool verbose = false;
};

void performBinding(BindContext &context, uint8_t segmentIndex, uint64_t segmentOffset,
                    uint8_t type, const std::string &symbolName, int64_t addend,
                    bool weakImport, bool isLazy) {
    constexpr size_t pointerSize = sizeof(uint64_t);
    if (type != BIND_TYPE_POINTER) {
        throw std::runtime_error("Unsupported bind type");
    }

    uint8_t *location = segmentAddress(context.segments, segmentIndex, segmentOffset, pointerSize);
    uint64_t resolvedValue = 0;

    void *resolved = context.resolver ? context.resolver(symbolName) : nullptr;
    if (!resolved && !symbolName.empty() && symbolName.front() == '_') {
        const std::string trimmed = symbolName.substr(1);
        resolved = context.resolver ? context.resolver(trimmed) : nullptr;
    }

    if (!resolved) {
        if (weakImport || isLazy) {
            resolvedValue = 0;
        } else {
            throw std::runtime_error("Failed to resolve symbol: " + symbolName);
        }
    } else {
        resolvedValue = reinterpret_cast<uint64_t>(resolved);
    }

    resolvedValue += static_cast<int64_t>(addend);
    auto *value = reinterpret_cast<uint64_t *>(location);
    *value = resolvedValue;
}

void applyBindOpcodes(BindContext &context, const uint8_t *opcodes, size_t size, bool isWeak,
                      bool isLazy) {
    if (size == 0) {
        return;
    }

    const uint8_t *cursor = opcodes;
    const uint8_t *end = opcodes + size;
    constexpr size_t pointerSize = sizeof(uint64_t);

    uint8_t segmentIndex = 0;
    uint64_t segmentOffset = 0;
    uint8_t type = BIND_TYPE_POINTER;
    int64_t addend = 0;
    std::string symbolName;
    uint8_t symbolFlags = 0;

    while (cursor < end) {
        const uint8_t byte = *cursor++;
        const uint8_t opcode = byte & BIND_OPCODE_MASK;
        const uint8_t immediate = byte & BIND_IMMEDIATE_MASK;

        switch (opcode) {
        case BIND_OPCODE_DONE:
            return;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
        case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
        case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
            if (opcode == BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB) {
                (void)readULEB128(cursor, end);
            }
            break;
        case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM: {
            const char *start = reinterpret_cast<const char *>(cursor);
            const char *terminator =
                static_cast<const char *>(memchr(start, '\0', static_cast<size_t>(end - cursor)));
            if (!terminator) {
                throw std::runtime_error("Unterminated bind symbol string");
            }
            symbolName.assign(start, static_cast<size_t>(terminator - start));
            cursor = reinterpret_cast<const uint8_t *>(terminator + 1);
            symbolFlags = immediate;
            addend = 0;
            break;
        }
        case BIND_OPCODE_SET_TYPE_IMM:
            type = immediate;
            break;
        case BIND_OPCODE_SET_ADDEND_SLEB:
            addend = readSLEB128(cursor, end);
            break;
        case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
            segmentIndex = immediate;
            segmentOffset = readULEB128(cursor, end);
            break;
        case BIND_OPCODE_ADD_ADDR_ULEB:
            segmentOffset += readULEB128(cursor, end);
            break;
        case BIND_OPCODE_DO_BIND: {
            const bool weakImport = (symbolFlags & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0;
            performBinding(context, segmentIndex, segmentOffset, type, symbolName, addend,
                           weakImport || isWeak, isLazy);
            segmentOffset += pointerSize;
            break;
        }
        case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB: {
            const bool weakImport = (symbolFlags & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0;
            performBinding(context, segmentIndex, segmentOffset, type, symbolName, addend,
                           weakImport || isWeak, isLazy);
            segmentOffset += readULEB128(cursor, end) + pointerSize;
            break;
        }
        case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED: {
            const bool weakImport = (symbolFlags & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0;
            performBinding(context, segmentIndex, segmentOffset, type, symbolName, addend,
                           weakImport || isWeak, isLazy);
            segmentOffset += static_cast<uint64_t>(immediate) * pointerSize + pointerSize;
            break;
        }
        case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: {
            const uint64_t count = readULEB128(cursor, end);
            const uint64_t skip = readULEB128(cursor, end);
            const bool weakImport = (symbolFlags & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0;
            for (uint64_t i = 0; i < count; ++i) {
                performBinding(context, segmentIndex, segmentOffset, type, symbolName, addend,
                               weakImport || isWeak, isLazy);
                segmentOffset += skip + pointerSize;
            }
            break;
        }
        case BIND_OPCODE_THREADED:
            throw std::runtime_error("Threaded binds are not supported in this prototype");
        default:
            throw std::runtime_error("Unknown bind opcode");
        }
    }
}

struct ImportEntry {
    uint64_t address = 0;
    bool weak = false;
    int64_t addend = 0;
};

const char *readCString(const uint8_t *base, size_t offset, size_t maxSize) {
    if (offset >= maxSize) {
        throw std::runtime_error("Import name offset exceeds fixup data");
    }

    const char *str = reinterpret_cast<const char *>(base + offset);
    size_t remaining = maxSize - offset;
    const void *terminator = memchr(str, '\0', remaining);
    if (!terminator) {
        throw std::runtime_error("Import name not NUL-terminated");
    }
    return str;
}

void populateImportTable(const LoaderOptions &options, const uint8_t *fixupBase,
                         size_t fixupSize, const dyld_chained_fixups_header *header,
                         std::vector<ImportEntry> &imports) {
    if (header->imports_count == 0) {
        return;
    }

    const uint32_t importsOffset = header->imports_offset;
    const uint32_t symbolsOffset = header->symbols_offset;
    if (importsOffset > fixupSize) {
        throw std::runtime_error("Imports offset exceeds fixup data size");
    }
    if (symbolsOffset > fixupSize) {
        throw std::runtime_error("Symbols offset exceeds fixup data size");
    }

    const size_t importsRegion = fixupSize - importsOffset;
    const size_t symbolsRegion = fixupSize - symbolsOffset;
    const uint8_t *importsBase = fixupBase + importsOffset;
    const uint8_t *symbolsBase = fixupBase + symbolsOffset;

    imports.resize(header->imports_count);
    SymbolResolver resolver = selectResolver(options);

    for (uint32_t i = 0; i < header->imports_count; ++i) {
        ImportEntry entry{};
        switch (header->imports_format) {
        case DYLD_CHAINED_IMPORT: {
            if ((i + 1) * sizeof(dyld_chained_import) > importsRegion) {
                throw std::runtime_error("Import table truncated");
            }
            const auto &rec =
                reinterpret_cast<const dyld_chained_import *>(importsBase)[i];
            const char *name =
                readCString(symbolsBase, rec.name_offset, symbolsRegion);
            void *resolved = resolver ? resolver(name) : nullptr;
            if (!resolved && name[0] == '_' && resolver) {
                resolved = resolver(name + 1);
            }
            if (!resolved && !rec.weak_import) {
                throw std::runtime_error(std::string("Failed to resolve import: ") + name);
            }
            entry.address = reinterpret_cast<uint64_t>(resolved);
            entry.weak = rec.weak_import;
            break;
        }
        case DYLD_CHAINED_IMPORT_ADDEND: {
            const size_t needed = (i + 1) * sizeof(dyld_chained_import_addend);
            if (needed > importsRegion) {
                throw std::runtime_error("Import addend table truncated");
            }
            const auto &rec =
                reinterpret_cast<const dyld_chained_import_addend *>(importsBase)[i];
            const char *name =
                readCString(symbolsBase, rec.name_offset, symbolsRegion);
            void *resolved = resolver ? resolver(name) : nullptr;
            if (!resolved && name[0] == '_' && resolver) {
                resolved = resolver(name + 1);
            }
            if (!resolved && !rec.weak_import) {
                throw std::runtime_error(std::string("Failed to resolve import: ") + name);
            }
            entry.address = reinterpret_cast<uint64_t>(resolved);
            entry.weak = rec.weak_import;
            entry.addend = rec.addend;
            break;
        }
        case DYLD_CHAINED_IMPORT_ADDEND64: {
            const size_t needed = (i + 1) * sizeof(dyld_chained_import_addend64);
            if (needed > importsRegion) {
                throw std::runtime_error("Import addend64 table truncated");
            }
            const auto &rec =
                reinterpret_cast<const dyld_chained_import_addend64 *>(importsBase)[i];
            const char *name =
                readCString(symbolsBase, rec.name_offset, symbolsRegion);
            void *resolved = resolver ? resolver(name) : nullptr;
            if (!resolved && name[0] == '_' && resolver) {
                resolved = resolver(name + 1);
            }
            if (!resolved && !rec.weak_import) {
                throw std::runtime_error(std::string("Failed to resolve import: ") + name);
            }
            entry.address = reinterpret_cast<uint64_t>(resolved);
            entry.weak = rec.weak_import;
            entry.addend = static_cast<int64_t>(rec.addend);
            break;
        }
        default:
            throw std::runtime_error("Unsupported chained import format");
        }

        imports[i] = entry;
    }
}

void applyChainedFixups(std::vector<SegmentRuntime> &segments, const uint8_t *fileData,
                        size_t fileSize, const linkedit_data_command *cmd,
                        ptrdiff_t slide, const LoaderOptions &options) {
    if (!cmd || cmd->datasize == 0) {
        return;
    }

    const uint32_t dataOffset = cmd->dataoff;
    const uint32_t dataSize = cmd->datasize;
    if (static_cast<uint64_t>(dataOffset) + static_cast<uint64_t>(dataSize) > fileSize) {
        throw std::runtime_error("Chained fixups exceed file bounds");
    }

    const uint8_t *fixupBase = fileData + dataOffset;
    const auto *header = reinterpret_cast<const dyld_chained_fixups_header *>(fixupBase);

    if (header->fixups_version != 0) {
        throw std::runtime_error("Unsupported chained fixups version");
    }
    if (header->symbols_format != 0) {
        throw std::runtime_error("Unsupported chained symbols format");
    }

    std::vector<ImportEntry> imports;
    populateImportTable(options, fixupBase, dataSize, header, imports);

    const uint32_t startsOffset = header->starts_offset;
    if (startsOffset >= dataSize) {
        throw std::runtime_error("Chained starts offset out of range");
    }
    const auto *starts =
        reinterpret_cast<const dyld_chained_starts_in_image *>(fixupBase + startsOffset);

    const uint32_t segCount = starts->seg_count;
    const uint8_t *startsBase = reinterpret_cast<const uint8_t *>(starts);

    for (uint32_t segIndex = 0; segIndex < segCount; ++segIndex) {
        if (segIndex >= segments.size()) {
            break;
        }
        uint32_t segInfoOffset = starts->seg_info_offset[segIndex];
        if (segInfoOffset == 0) {
            continue;
        }
        if (startsOffset + segInfoOffset >= dataSize) {
            throw std::runtime_error("Segment info offset out of range");
        }

        const auto *segInfo =
            reinterpret_cast<const dyld_chained_starts_in_segment *>(startsBase + segInfoOffset);

        if (!segments[segIndex].mapped || !segments[segIndex].command) {
            continue;
        }

        uint16_t pageSize = segInfo->page_size;
        if (pageSize == 0) {
            pageSize = 0x1000;
        }

        if (segInfo->pointer_format != DYLD_CHAINED_PTR_64 &&
            segInfo->pointer_format != DYLD_CHAINED_PTR_64_OFFSET) {
            throw std::runtime_error("Unsupported chained pointer format");
        }

        const uint16_t pageCount = segInfo->page_count;
        const uint16_t *pageStarts = segInfo->page_start;

        for (uint16_t page = 0; page < pageCount; ++page) {
            uint16_t start = pageStarts[page];
            if (start == DYLD_CHAINED_PTR_START_NONE) {
                continue;
            }
            if (start & DYLD_CHAINED_PTR_START_MULTI) {
                throw std::runtime_error("Multi-start chained fixups not supported");
            }

            uint8_t *pageBase =
                segments[segIndex].mapped + static_cast<uint64_t>(page) * pageSize;
            if (start >= pageSize) {
                throw std::runtime_error("Chained fixup start exceeds page size");
            }

            uint8_t *chainLocation = pageBase + start;
            while (true) {
                auto *slot = reinterpret_cast<uint64_t *>(chainLocation);
                uint64_t raw = *slot;
                bool isBind = (raw >> 63) & 0x1;
                uint16_t next = static_cast<uint16_t>((raw >> 51) & 0xFFF);

                if (isBind) {
                    uint32_t ordinal = static_cast<uint32_t>(raw & 0xFFFFFF);
                    uint64_t addend = (raw >> 24) & 0xFF;
                    if (ordinal >= imports.size()) {
                        throw std::runtime_error("Chained bind ordinal out of range");
                    }
                    const ImportEntry &import = imports[ordinal];
                    uint64_t value = import.address;
                    if (!value && !import.weak) {
                        throw std::runtime_error("Unresolved non-weak import in chained fixups");
                    }
                    value += addend;
                    value += static_cast<uint64_t>(import.addend);
                    *slot = value;
                } else {
                    uint64_t target = raw & ((1ULL << 36) - 1ULL);
                    uint64_t high8 = (raw >> 36) & 0xFFULL;
                    uint64_t result = (high8 << 56) | target;
                    if (segInfo->pointer_format == DYLD_CHAINED_PTR_64_OFFSET) {
                        result += static_cast<uint64_t>(slide);
                    } else {
                        result += static_cast<uint64_t>(slide);
                    }
                    *slot = result;
                }

                if (next == 0) {
                    break;
                }
                chainLocation += static_cast<size_t>(next) * 4;

                const uint8_t *segmentStart = segments[segIndex].mapped;
                size_t offsetInSegment =
                    static_cast<size_t>(chainLocation - segmentStart);
                if (offsetInSegment >= segments[segIndex].command->vmsize) {
                    throw std::runtime_error("Chained fixup overflow beyond segment");
                }
            }
        }
    }
}

int vmProtToMmapProt(vm_prot_t prot) {
    int result = 0;
    if (prot & VM_PROT_READ) {
        result |= PROT_READ;
    }
    if (prot & VM_PROT_WRITE) {
        result |= PROT_WRITE;
    }
    if (prot & VM_PROT_EXECUTE) {
        result |= PROT_EXEC;
    }
    return result;
}

bool isPageZeroSegment(const segment_command_64 &seg) {
    return std::strcmp(seg.segname, "__PAGEZERO") == 0;
}

LoadedImage loadMachOImage(const uint8_t *data, size_t size, const LoaderOptions &options);

LoadedImage loadThinMachO(const uint8_t *data, size_t size, const LoaderOptions &options) {
    if (size < sizeof(mach_header_64)) {
        throw std::runtime_error("Buffer too small for mach_header_64");
    }

    const auto *header = reinterpret_cast<const mach_header_64 *>(data);
    if (header->magic != MH_MAGIC_64) {
        throw std::runtime_error("Unsupported Mach-O magic (expected MH_MAGIC_64)");
    }

    const uint8_t *cursor = data + sizeof(mach_header_64);
    const uint8_t *const end = data + size;

    std::vector<SegmentRuntime> segments;
    segments.reserve(header->ncmds);

    const symtab_command *symtab = nullptr;
    const dyld_info_command *dyldInfo = nullptr;
    const linkedit_data_command *chainedFixups = nullptr;

    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (cursor + sizeof(load_command) > end) {
            throw std::runtime_error("Load command exceeds buffer bounds");
        }

        const auto *lc = reinterpret_cast<const load_command *>(cursor);
        if (lc->cmdsize == 0 || cursor + lc->cmdsize > end) {
            throw std::runtime_error("Invalid load command size");
        }

        if (lc->cmd == LC_SEGMENT_64) {
            if (lc->cmdsize < sizeof(segment_command_64)) {
                throw std::runtime_error("LC_SEGMENT_64 truncated");
            }
            const auto *seg = reinterpret_cast<const segment_command_64 *>(cursor);
            segments.push_back({seg, nullptr});
        } else if (lc->cmd == LC_SYMTAB) {
            if (lc->cmdsize != sizeof(symtab_command)) {
                throw std::runtime_error("LC_SYMTAB has unexpected size");
            }
            symtab = reinterpret_cast<const symtab_command *>(cursor);
        } else if (lc->cmd == LC_DYLD_INFO || lc->cmd == LC_DYLD_INFO_ONLY) {
            if (lc->cmdsize != sizeof(dyld_info_command)) {
                throw std::runtime_error("LC_DYLD_INFO has unexpected size");
            }
            dyldInfo = reinterpret_cast<const dyld_info_command *>(cursor);
        } else if (lc->cmd == LC_DYLD_CHAINED_FIXUPS) {
            if (lc->cmdsize != sizeof(linkedit_data_command)) {
                throw std::runtime_error("LC_DYLD_CHAINED_FIXUPS has unexpected size");
            }
            chainedFixups = reinterpret_cast<const linkedit_data_command *>(cursor);
        }

        cursor += lc->cmdsize;
    }

    if (segments.empty()) {
        throw std::runtime_error("Mach-O contains no segments");
    }

    constexpr uint64_t kMaxAddr = std::numeric_limits<uint64_t>::max();
    uint64_t minVmaddr = kMaxAddr;
    uint64_t maxVmaddr = 0;

    for (const auto &seg : segments) {
        const segment_command_64 *command = seg.command;
        if (!command || command->vmsize == 0) {
            continue;
        }
        if (isPageZeroSegment(*command)) {
            continue;
        }
        if (command->vmaddr < minVmaddr) {
            minVmaddr = command->vmaddr;
        }
        const uint64_t segEnd = command->vmaddr + command->vmsize;
        if (segEnd > maxVmaddr) {
            maxVmaddr = segEnd;
        }
    }

    if (minVmaddr == kMaxAddr || maxVmaddr <= minVmaddr) {
        throw std::runtime_error("Unable to determine Mach-O VM range");
    }

    const size_t pageSize = static_cast<size_t>(getpagesize());
    const uint64_t imageSpan = maxVmaddr - minVmaddr;
    const size_t imageSize =
        static_cast<size_t>((imageSpan + pageSize - 1) & static_cast<uint64_t>(~(pageSize - 1)));

    void *mapping = mmap(nullptr, imageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) {
        throw std::runtime_error("mmap failed: " + std::string(std::strerror(errno)));
    }

    uint8_t *base = reinterpret_cast<uint8_t *>(mapping);
    const ptrdiff_t slide = reinterpret_cast<uintptr_t>(base) - static_cast<ptrdiff_t>(minVmaddr);

    for (std::size_t i = 0; i < segments.size(); ++i) {
        const segment_command_64 *seg = segments[i].command;
        if (!seg || seg->vmsize == 0 || isPageZeroSegment(*seg)) {
            continue;
        }

        if (seg->fileoff + seg->filesize > size) {
            munmap(base, imageSize);
            throw std::runtime_error("Segment exceeds file bounds");
        }

        uint8_t *segmentDest = base + (seg->vmaddr - minVmaddr);
        segments[i].mapped = segmentDest;

        if (seg->filesize > 0) {
            const uint8_t *src = data + seg->fileoff;
            std::memcpy(segmentDest, src, static_cast<size_t>(seg->filesize));
        }

        if (seg->filesize < seg->vmsize) {
            std::memset(segmentDest + seg->filesize, 0,
                        static_cast<size_t>(seg->vmsize - seg->filesize));
        }

        // defer setting final protections until after rebasing/binding
    }

    if (dyldInfo) {
        if (dyldInfo->rebase_size > 0) {
            const uint8_t *rebaseStart = data + dyldInfo->rebase_off;
            if (dyldInfo->rebase_off + dyldInfo->rebase_size > size) {
                munmap(base, imageSize);
                throw std::runtime_error("Rebase info exceeds file bounds");
            }
            applyRebaseOpcodes(segments, rebaseStart, dyldInfo->rebase_size, slide);
        }

        SymbolResolver resolver = selectResolver(options);
        BindContext bindCtx{segments, resolver, options.verbose};

        if (dyldInfo->bind_size > 0) {
            const uint8_t *bindStart = data + dyldInfo->bind_off;
            if (dyldInfo->bind_off + dyldInfo->bind_size > size) {
                munmap(base, imageSize);
                throw std::runtime_error("Bind info exceeds file bounds");
            }
            applyBindOpcodes(bindCtx, bindStart, dyldInfo->bind_size, false, false);
        }

        if (dyldInfo->weak_bind_size > 0) {
            const uint8_t *weakStart = data + dyldInfo->weak_bind_off;
            if (dyldInfo->weak_bind_off + dyldInfo->weak_bind_size > size) {
                munmap(base, imageSize);
                throw std::runtime_error("Weak bind info exceeds file bounds");
            }
            applyBindOpcodes(bindCtx, weakStart, dyldInfo->weak_bind_size, true, false);
        }

        if (dyldInfo->lazy_bind_size > 0) {
            const uint8_t *lazyStart = data + dyldInfo->lazy_bind_off;
            if (dyldInfo->lazy_bind_off + dyldInfo->lazy_bind_size > size) {
                munmap(base, imageSize);
                throw std::runtime_error("Lazy bind info exceeds file bounds");
            }
            applyBindOpcodes(bindCtx, lazyStart, dyldInfo->lazy_bind_size, false, true);
        }
    }

    applyChainedFixups(segments, data, size, chainedFixups, slide, options);

    for (const auto &segmentInfo : segments) {
        const segment_command_64 *seg = segmentInfo.command;
        if (!seg || seg->vmsize == 0 || isPageZeroSegment(*seg)) {
            continue;
        }

        uint8_t *segmentDest = segmentInfo.mapped;
        const uintptr_t segBegin = reinterpret_cast<uintptr_t>(segmentDest);
        const uintptr_t segEnd = segBegin + static_cast<size_t>(seg->vmsize);
        const uintptr_t protBegin = segBegin & ~(static_cast<uintptr_t>(pageSize) - 1);
        const uintptr_t protEnd =
            (segEnd + static_cast<uintptr_t>(pageSize) - 1) & ~(static_cast<uintptr_t>(pageSize) - 1);
        const size_t protSize = static_cast<size_t>(protEnd - protBegin);
        int prot = vmProtToMmapProt(seg->initprot);
        if (protSize != 0) {
            if (mprotect(reinterpret_cast<void *>(protBegin), protSize,
                         prot == 0 ? PROT_NONE : prot) != 0) {
                munmap(base, imageSize);
                throw std::runtime_error("mprotect failed for segment " +
                                         std::string(seg->segname) + ": " +
                                         std::strerror(errno));
            }
        }
    }

    std::unordered_map<std::string, void *> symbols;
    if (symtab) {
        const uint64_t symEnd =
            static_cast<uint64_t>(symtab->symoff) +
            static_cast<uint64_t>(symtab->nsyms) * sizeof(nlist_64);
        const uint64_t strEnd =
            static_cast<uint64_t>(symtab->stroff) + static_cast<uint64_t>(symtab->strsize);
        if (symEnd > size || strEnd > size) {
            munmap(base, imageSize);
            throw std::runtime_error("Symbol or string table exceeds file bounds");
        }

        const auto *symbolsTable =
            reinterpret_cast<const nlist_64 *>(data + symtab->symoff);
        const auto *stringTable = reinterpret_cast<const char *>(data + symtab->stroff);

        for (uint32_t i = 0; i < symtab->nsyms; ++i) {
            const nlist_64 &sym = symbolsTable[i];
            if ((sym.n_type & N_EXT) == 0) {
                continue;
            }
            if ((sym.n_type & N_TYPE) != N_SECT) {
                continue;
            }
            if (sym.n_value == 0) {
                continue;
            }
            if (sym.n_un.n_strx == 0 || sym.n_un.n_strx >= symtab->strsize) {
                continue;
            }

            const char *nameCStr = stringTable + sym.n_un.n_strx;
            if (*nameCStr == '\0') {
                continue;
            }

            const uintptr_t runtimeAddr =
                static_cast<uintptr_t>(sym.n_value + slide);

            std::string name{nameCStr};
            symbols.emplace(name, reinterpret_cast<void *>(runtimeAddr));

            if (!name.empty() && name.front() == '_' && name.size() > 1) {
                symbols.emplace(name.substr(1), reinterpret_cast<void *>(runtimeAddr));
            }
        }
    }

    return LoadedImage(base, imageSize, slide, std::move(symbols));
}

LoadedImage loadFatMachO(const uint8_t *data, size_t size, const LoaderOptions &options) {
    if (size < sizeof(fat_header)) {
        throw std::runtime_error("Buffer too small for fat_header");
    }

    const auto *fat = reinterpret_cast<const fat_header *>(data);
    const uint32_t nfat = OSSwapBigToHostInt32(fat->nfat_arch);
    if (nfat == 0) {
        throw std::runtime_error("Fat Mach-O reports zero architectures");
    }

#if defined(__aarch64__) || defined(__arm64__)
    constexpr cpu_type_t preferredCpu = CPU_TYPE_ARM64;
#elif defined(__x86_64__)
    constexpr cpu_type_t preferredCpu = CPU_TYPE_X86_64;
#else
    constexpr cpu_type_t preferredCpu = CPU_TYPE_ANY;
#endif

    const auto *archs = reinterpret_cast<const fat_arch *>(data + sizeof(fat_header));
    const fat_arch *selectedArch = nullptr;

    for (uint32_t i = 0; i < nfat; ++i) {
        const auto &arch = archs[i];
        const cpu_type_t cpu = static_cast<cpu_type_t>(OSSwapBigToHostInt32(arch.cputype));
        if (preferredCpu != CPU_TYPE_ANY && cpu == preferredCpu) {
            selectedArch = &arch;
            break;
        }
    }

    if (!selectedArch) {
        selectedArch = &archs[0];
    }

    const uint32_t offset = OSSwapBigToHostInt32(selectedArch->offset);
    const uint32_t sliceSize = OSSwapBigToHostInt32(selectedArch->size);
    if (offset + sliceSize > size || sliceSize == 0) {
        throw std::runtime_error("Invalid architecture slice in fat Mach-O");
    }

    return loadMachOImage(data + offset, sliceSize, options);
}

LoadedImage loadMachOImage(const uint8_t *data, size_t size, const LoaderOptions &options) {
    if (size < sizeof(uint32_t)) {
        throw std::runtime_error("Buffer too small for Mach-O magic");
    }

    const uint32_t magic = *reinterpret_cast<const uint32_t *>(data);
    switch (magic) {
    case MH_MAGIC_64:
        return loadThinMachO(data, size, options);
    case FAT_MAGIC:
    case FAT_CIGAM:
        return loadFatMachO(data, size, options);
    default:
        throw std::runtime_error("Unsupported Mach-O magic: 0x" + std::to_string(magic));
    }
}

} // namespace

LoadedImage::LoadedImage(uint8_t *base, size_t size, ptrdiff_t slide,
                         std::unordered_map<std::string, void *> symbols)
    : base_(base), size_(size), slide_(slide), symbols_(std::move(symbols)) {}

LoadedImage::~LoadedImage() { unloadMachOImage(*this); }

LoadedImage::LoadedImage(LoadedImage &&other) noexcept { *this = std::move(other); }

LoadedImage &LoadedImage::operator=(LoadedImage &&other) noexcept {
    if (this != &other) {
        unloadMachOImage(*this);
        base_ = other.base_;
        size_ = other.size_;
        slide_ = other.slide_;
        symbols_ = std::move(other.symbols_);

        other.base_ = nullptr;
        other.size_ = 0;
        other.slide_ = 0;
        other.symbols_.clear();
    }
    return *this;
}

void *LoadedImage::findSymbol(const std::string &name) const {
    if (!base_) {
        return nullptr;
    }

    const auto it = symbols_.find(name);
    if (it != symbols_.end()) {
        return it->second;
    }
    return nullptr;
}

LoadedImage loadMachOImage(const std::string &path) {
    return loadMachOImage(path, LoaderOptions{});
}

LoadedImage loadMachOImage(const std::string &path, const LoaderOptions &options) {
    const auto buffer = readFile(path);
    return loadMachOImage(buffer.data(), buffer.size(), options);
}

LoadedImage loadMachOImageFromBuffer(const uint8_t *data, size_t size) {
    return loadMachOImageFromBuffer(data, size, LoaderOptions{});
}

LoadedImage loadMachOImageFromBuffer(const uint8_t *data, size_t size,
                                     const LoaderOptions &options) {
    return loadMachOImage(data, size, options);
}

void unloadMachOImage(LoadedImage &image) {
    if (image.base_) {
        munmap(image.base_, image.size_);
    }
    image.base_ = nullptr;
    image.size_ = 0;
    image.slide_ = 0;
    image.symbols_.clear();
}
