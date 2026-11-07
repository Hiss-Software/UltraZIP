#pragma once
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace uzip {

enum class Method : uint8_t {
    Zstd  = 0,
    LZMA2 = 1,
    Store = 2,
    Auto  = 3,  // brute-force: tries all methods, keeps the best
};

enum class Level : uint8_t {
    Fast   = 1,
    Normal = 5,
    Max    = 19,
    Ultra  = 22,
};

enum class Filter : uint8_t {
    None  = 0,
    BCJ   = 1,   // x86 branch convert (exe, dll)
    Delta = 2,   // delta filter (raw binary)
    Auto  = 3,   // auto-detect by content
};

struct Header {
    static constexpr char MAGIC_V1[9] = {'U','L','T','R','A','Z','I','P','\x01'};
    static constexpr char MAGIC_V2[9] = {'U','L','T','R','A','Z','I','P','\x02'};
    static constexpr uint16_t VERSION = 2;

    char      magic[9];
    uint16_t  version;
    uint8_t   method;
    uint8_t   level;
    uint8_t   solid;
    uint8_t   filter;     // NEW (was reserved[0] in v1, always 0 — backward compatible)
    uint8_t   reserved[2];
    uint32_t  fileCount;
    uint64_t  totalOriginal;
    uint64_t  totalCompressed;
};

struct FileEntry {
    std::string name;
    uint64_t originalSize;
    uint64_t offsetInSolid;
    uint64_t compressedSize;
};

bool create(const std::string& archivePath,
            const std::vector<std::string>& filePaths,
            Method method = Method::Auto,
            Level  level  = Level::Ultra,
            bool   solid  = true,
            Filter filter = Filter::Auto);

bool extract(const std::string& archivePath,
             const std::string& outputDir = ".");

std::vector<FileEntry> list(const std::string& archivePath);

} // namespace uzip