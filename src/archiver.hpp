#pragma once
#include <string>
#include <vector>

namespace archiver {

struct Entry {
    std::string name;
    long long    size;
    bool         isDir;
};

bool pack(const std::string& archivePath,
          const std::vector<std::string>& filePaths,
          const std::string& filter = "");

bool unpack(const std::string& archivePath,
            const std::string& outputDir = ".");

std::vector<Entry> listEntries(const std::string& archivePath);

bool isSupported(const std::string& archivePath);

} // namespace archiver
