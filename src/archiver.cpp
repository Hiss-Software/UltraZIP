#include "archiver.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <iostream>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <fci.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using namespace archiver;

// ═══════════════════════════════════════════════════════════
//  Utilities
// ═══════════════════════════════════════════════════════════

static std::string fullExtLower(const std::string& archivePath) {
    fs::path p(archivePath);
    std::string fn = p.filename().string();
    auto dot = fn.find('.');
    if (dot == std::string::npos) return "";
    std::string ext = fn.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return ext;
}

static void writeLE16(std::ofstream& out, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
    out.write((char*)b, 2);
}
static void writeLE32(std::ofstream& out, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8),
                    (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    out.write((char*)b, 4);
}

// ═══════════════════════════════════════════════════════════
//  LHA / LZH — native writer (level 2 header, -lh0- store)
// ═══════════════════════════════════════════════════════════

static uint16_t lhaCRC16(const uint8_t* data, size_t len) {
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}

static void toDOSDateTime(time_t t, uint16_t* dosDate, uint16_t* dosTime) {
    struct tm* lt = localtime(&t);
    if (!lt) { *dosDate = 0; *dosTime = 0; return; }
    *dosDate = (uint16_t)((lt->tm_mday) | ((lt->tm_mon + 1) << 5) |
              ((lt->tm_year - 80) << 9));
    *dosTime = (uint16_t)((lt->tm_sec / 2) | (lt->tm_min << 5) |
              (lt->tm_hour << 11));
}

static bool packLHA(const std::string& archivePath,
                    const std::vector<std::string>& filePaths)
{
    std::ofstream out(archivePath, std::ios::binary);
    if (!out) {
        std::cerr << "[archiver] Failed to create: " << archivePath << "\n";
        return false;
    }

    for (const auto& fp : filePaths) {
        if (!fs::exists(fp)) {
            std::cerr << "[archiver] Skipping (not found): " << fp << "\n";
            continue;
        }

        std::ifstream f(fp, std::ios::binary);
        if (!f) continue;

        std::vector<uint8_t> fileData((std::istreambuf_iterator<char>(f)),
                                       std::istreambuf_iterator<char>());
        f.close();

        std::string name = fs::path(fp).filename().string();
        uint32_t origSize = (uint32_t)fileData.size();
        uint32_t compSize = origSize;
        uint16_t crc = lhaCRC16(fileData.data(), fileData.size());

        auto ft = fs::last_write_time(fp);
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        time_t mtime = std::chrono::system_clock::to_time_t(sctp);
        uint16_t dosDate, dosTime;
        toDOSDateTime(mtime, &dosDate, &dosTime);

        uint16_t headerSize = (uint16_t)(22 + name.size());

        writeLE16(out, headerSize);
        out.write("-lh0-", 5);
        writeLE32(out, compSize);
        writeLE32(out, origSize);
        writeLE16(out, dosDate);
        writeLE16(out, dosTime);
        uint8_t attr = 0x20;
        out.write((char*)&attr, 1);
        uint8_t level = 2;
        out.write((char*)&level, 1);
        uint8_t nameLen = (uint8_t)name.size();
        out.write((char*)&nameLen, 1);
        out.write(name.data(), nameLen);
        writeLE16(out, crc);
        uint8_t osId = 'W';
        out.write((char*)&osId, 1);
        writeLE16(out, 0x0000);

        out.write((char*)fileData.data(), fileData.size());
    }

    writeLE16(out, 0x0000);
    out.close();
    return true;
}

// ═══════════════════════════════════════════════════════════
//  CAB — native Windows FCI API (fixed for MinGW-w64)
// ═══════════════════════════════════════════════════════════
#ifdef _WIN32

// MinGW-w64 uses DIAMONDAPI and int *err in callbacks

static void* DIAMONDAPI fnMemAlloc(ULONG size) { return malloc(size); }
static void DIAMONDAPI fnMemFree(void* mem) { free(mem); }

static INT_PTR DIAMONDAPI fnFileOpen(char* path, int oflag, int pmode,
                                     int* err, void* ctx) {
    (void)pmode; (void)ctx; (void)err;
    DWORD access = GENERIC_READ;
    if (oflag & _O_WRONLY) access = GENERIC_WRITE;
    else if (oflag & _O_RDWR) access = GENERIC_READ | GENERIC_WRITE;
    DWORD disp = (oflag & _O_CREAT) ? CREATE_ALWAYS : OPEN_EXISTING;
    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ, NULL, disp,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { if (err) *err = -1; return -1; }
    if (err) *err = 0;
    return (INT_PTR)h;
}

static UINT DIAMONDAPI fnFileRead(INT_PTR hf, void* mem, UINT cb,
                                  int* err, void* ctx) {
    (void)ctx; (void)err;
    DWORD rd = 0;
    if (!ReadFile((HANDLE)hf, mem, cb, &rd, NULL)) {
        if (err) *err = -1;
        return (UINT)-1;
    }
    if (err) *err = 0;
    return rd;
}

static UINT DIAMONDAPI fnFileWrite(INT_PTR hf, void* mem, UINT cb,
                                   int* err, void* ctx) {
    (void)ctx; (void)err;
    DWORD wr = 0;
    if (!WriteFile((HANDLE)hf, mem, cb, &wr, NULL)) {
        if (err) *err = -1;
        return (UINT)-1;
    }
    if (err) *err = 0;
    return wr;
}

static int DIAMONDAPI fnFileClose(INT_PTR hf, int* err, void* ctx) {
    (void)ctx; (void)err;
    BOOL ok = CloseHandle((HANDLE)hf);
    if (err) *err = ok ? 0 : -1;
    return ok ? 0 : -1;
}

static long DIAMONDAPI fnFileSeek(INT_PTR hf, long dist, int seektype,
                                  int* err, void* ctx) {
    (void)ctx; (void)err;
    DWORD method = (seektype == SEEK_CUR) ? FILE_CURRENT :
                   (seektype == SEEK_END) ? FILE_END : FILE_BEGIN;
    LONG hi = 0;
    DWORD lo = SetFilePointer((HANDLE)hf, dist, &hi, method);
    if (lo == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) {
        if (err) *err = -1;
        return -1;
    }
    if (err) *err = 0;
    return (long)lo;
}

static int DIAMONDAPI fnFileDelete(char* path, int* err, void* ctx) {
    (void)ctx; (void)err;
    BOOL ok = DeleteFileA(path);
    if (err) *err = ok ? 0 : -1;
    return ok ? 0 : -1;
}

static BOOL DIAMONDAPI fnGetTempFileName(char* temp, int cbTemp, void* ctx) {
    (void)ctx;
    char dir[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    char name[MAX_PATH];
    if (!GetTempFileNameA(dir, "cab", 0, name)) return FALSE;
    strncpy(temp, name, cbTemp);
    return TRUE;
}

static BOOL DIAMONDAPI fnGetNextCabinet(PCCAB pccab, ULONG cbPrev, void* ctx) {
    (void)pccab; (void)cbPrev; (void)ctx;
    return FALSE;
}

static long DIAMONDAPI fnStatus(UINT status, ULONG cb1, ULONG cb2, void* ctx) {
    (void)status; (void)cb1; (void)cb2; (void)ctx;
    return 0;
}

static int DIAMONDAPI fnFilePlaced(PCCAB pccab, char* path, long cb,
                                   BOOL cont, void* ctx) {
    (void)pccab; (void)path; (void)cb; (void)cont; (void)ctx;
    return 0;
}

static INT_PTR DIAMONDAPI fnGetOpenInfo(char* name, USHORT* date,
                                        USHORT* time, USHORT* attr,
                                        int* err, void* ctx) {
    (void)ctx;
    HANDLE h = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { if (err) *err = -1; return -1; }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info)) {
        CloseHandle(h);
        if (err) *err = -1;
        return -1;
    }
    FILETIME local;
    FileTimeToLocalFileTime(&info.ftLastWriteTime, &local);
    FileTimeToDosDateTime(&local, (LPWORD)date, (LPWORD)time);
    *attr = (USHORT)(info.dwFileAttributes & (FILE_ATTRIBUTE_READONLY |
                     FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                     FILE_ATTRIBUTE_ARCHIVE));
    if (err) *err = 0;
    return (INT_PTR)h;
}

static bool packCAB(const std::string& archivePath,
                    const std::vector<std::string>& filePaths)
{
    ERF erf;
    CCAB ccab;
    ZeroMemory(&erf, sizeof(erf));
    ZeroMemory(&ccab, sizeof(ccab));

    fs::path p(archivePath);
    std::string dir = p.parent_path().string();
    std::string name = p.filename().string();
    if (dir.empty()) dir = ".";
    if (dir.back() != '\\') dir += '\\';

    // MinGW-w64: szCab = filename, szCabPath = path
    strncpy(ccab.szCabPath, dir.c_str(), sizeof(ccab.szCabPath) - 1);
    strncpy(ccab.szCab, name.c_str(), sizeof(ccab.szCab) - 1);
    ccab.cb = 0;
    ccab.cbFolderThresh = 0x7FFFFFFF;  // everything into one folder
    ccab.iCab = 1;
    ccab.iDisk = 0;
    ccab.setID = 0;

    HFCI hfci = FCICreate(&erf,
        fnFilePlaced, fnMemAlloc, fnMemFree,
        fnFileOpen, fnFileRead, fnFileWrite, fnFileClose,
        fnFileSeek, fnFileDelete,
        fnGetTempFileName, &ccab, NULL);

    if (!hfci) {
        std::cerr << "[archiver] FCICreate failed: " << erf.erfOper << "\n";
        return false;
    }

    bool ok = true;
    for (const auto& fp : filePaths) {
        if (!fs::exists(fp)) {
            std::cerr << "[archiver] Skipping (not found): " << fp << "\n";
            continue;
        }
        std::string fname = fs::path(fp).filename().string();
        std::string fpNative = fp;
        std::replace(fpNative.begin(), fpNative.end(), '/', '\\');
        std::replace(fname.begin(), fname.end(), '/', '\\');

        if (!FCIAddFile(hfci,
                        const_cast<char*>(fpNative.c_str()),
                        const_cast<char*>(fname.c_str()),
                        FALSE,
                        fnGetNextCabinet, fnStatus, fnGetOpenInfo,
                        tcompTYPE_MSZIP)) {
            std::cerr << "[archiver] FCIAddFile failed: " << erf.erfOper
                      << " for " << fp << "\n";
            ok = false;
        }
    }

    if (!FCIFlushCabinet(hfci, FALSE, fnGetNextCabinet, fnStatus)) {
        std::cerr << "[archiver] FCIFlushCabinet failed\n";
        ok = false;
    }
    FCIDestroy(hfci);
    return ok;
}

#else
static bool packCAB(const std::string& archivePath,
                    const std::vector<std::string>& filePaths)
{
    (void)archivePath; (void)filePaths;
    std::cerr << "[archiver] CAB is only supported on Windows (FCI API)\n";
    return false;
}
#endif

// ═══════════════════════════════════════════════════════════
//  libarchive: write formats
// ═══════════════════════════════════════════════════════════

static bool setWriteFormat(struct archive* a, const std::string& ext) {
    if (ext == "7z") {
        archive_write_set_format_7zip(a);
    } else if (ext == "zip" || ext == "jar" || ext == "apk" || ext == "whl") {
        archive_write_set_format_zip(a);
    } else if (ext == "tar") {
        archive_write_set_format_pax_restricted(a);
    } else if (ext == "tar.gz" || ext == "tgz") {
        archive_write_set_format_pax_restricted(a);
        archive_write_add_filter_gzip(a);
    } else if (ext == "tar.xz" || ext == "txz") {
        archive_write_set_format_pax_restricted(a);
        archive_write_add_filter_xz(a);
    } else if (ext == "tar.bz2" || ext == "tbz" || ext == "tbz2") {
        archive_write_set_format_pax_restricted(a);
        archive_write_add_filter_bzip2(a);
    } else if (ext == "tar.zst" || ext == "tzst") {
        archive_write_set_format_pax_restricted(a);
        archive_write_add_filter_zstd(a);
    } else if (ext == "tar.lz4" || ext == "tlz4") {
        archive_write_set_format_pax_restricted(a);
        archive_write_add_filter_lz4(a);
    } else if (ext == "tar.lzo" || ext == "tlzo") {
        archive_write_set_format_pax_restricted(a);
        archive_write_add_filter_lzop(a);
    } else if (ext == "tar.Z" || ext == "taz") {
        archive_write_set_format_pax_restricted(a);
        archive_write_add_filter_compress(a);
    } else if (ext == "gz") {
        archive_write_set_format_raw(a);
        archive_write_add_filter_gzip(a);
    } else if (ext == "xz") {
        archive_write_set_format_raw(a);
        archive_write_add_filter_xz(a);
    } else if (ext == "bz2") {
        archive_write_set_format_raw(a);
        archive_write_add_filter_bzip2(a);
    } else if (ext == "zst" || ext == "zstd") {
        archive_write_set_format_raw(a);
        archive_write_add_filter_zstd(a);
    } else if (ext == "lz4") {
        archive_write_set_format_raw(a);
        archive_write_add_filter_lz4(a);
    } else if (ext == "lzo" || ext == "lzop") {
        archive_write_set_format_raw(a);
        archive_write_add_filter_lzop(a);
    } else if (ext == "Z") {
        archive_write_set_format_raw(a);
        archive_write_add_filter_compress(a);
    } else if (ext == "ar" || ext == "deb") {
        archive_write_set_format_ar_bsd(a);
    } else if (ext == "cpio" || ext == "odc") {
        archive_write_set_format_cpio_newc(a);
    } else if (ext == "iso" || ext == "iso9660") {
        archive_write_set_format_iso9660(a);
    } else if (ext == "xar") {
        archive_write_set_format_xar(a);
    } else if (ext == "mtree") {
        archive_write_set_format_mtree(a);
    } else if (ext == "warc") {
        archive_write_set_format_warc(a);
    } else {
        archive_write_set_format_pax_restricted(a);
        archive_write_add_filter_xz(a);
        return false;
    }
    return true;
}

static void setCompressionOptions(struct archive* a, const std::string& ext,
                                  const std::string& filter)
{
    if (ext == "zip" || ext == "jar" || ext == "apk" || ext == "whl") {
        archive_write_set_options(a, "zip:compression-level=9"); return;
    }
    if (ext == "7z") {
        archive_write_set_options(a, "7zip:compression-level=9"); return;
    }
    if (ext == "gz" || ext == "tgz" || filter == "gz") {
        archive_write_set_options(a, "gzip:compression-level=9"); return;
    }
    if (ext == "xz" || ext == "txz" || filter == "xz") {
        archive_write_set_options(a, "xz:compression-level=9"); return;
    }
    if (ext == "zst" || ext == "zstd" || ext == "tzst" || filter == "zstd") {
        archive_write_set_options(a, "zstd:compression-level=22"); return;
    }
    if (ext == "bz2" || ext == "tbz" || ext == "tbz2" || filter == "bz2") {
        archive_write_set_options(a, "bzip2:compression-level=9"); return;
    }
    if (ext == "lz4" || ext == "tlz4" || filter == "lz4") {
        archive_write_set_options(a, "lz4:compression-level=9"); return;
    }
    if (ext == "lzo" || ext == "lzop" || ext == "tlzo" || filter == "lzo") {
        archive_write_set_options(a, "lzop:compression-level=9"); return;
    }
}

// ═══════════════════════════════════════════════════════════
//  Pack
// ═══════════════════════════════════════════════════════════

bool archiver::pack(const std::string& archivePath,
                    const std::vector<std::string>& filePaths,
                    const std::string& filter)
{
    std::string ext = fullExtLower(archivePath);

    if (ext == "lha" || ext == "lzh" || ext == "lzs") {
        bool ok = packLHA(archivePath, filePaths);
        if (ok) std::cout << "[archiver] Archive created: " << archivePath << "\n";
        return ok;
    }

    if (ext == "cab") {
        bool ok = packCAB(archivePath, filePaths);
        if (ok) std::cout << "[archiver] Archive created: " << archivePath << "\n";
        return ok;
    }

    if (ext == "rar" || ext == "r00") {
        std::cerr << "[archiver] RAR writing is not supported (read-only)\n";
        return false;
    }

    struct archive* a = archive_write_new();
    if (!a) {
        std::cerr << "[archiver] archive_write_new failed\n";
        return false;
    }

    bool knownFormat = setWriteFormat(a, ext);
    if (!knownFormat) {
        std::cerr << "[archiver] Unknown extension ." << ext
                  << " — using tar.xz as fallback\n";
    }

    setCompressionOptions(a, ext, filter);

    bool isRaw = (ext == "gz" || ext == "xz" || ext == "bz2" ||
                  ext == "zst" || ext == "zstd" || ext == "lz4" ||
                  ext == "lzo" || ext == "lzop" || ext == "Z");

    if (isRaw && filePaths.size() > 1) {
        std::cerr << "[archiver] Format ." << ext
                  << " supports only one file\n";
        archive_write_free(a);
        return false;
    }

    if (archive_write_open_filename(a, archivePath.c_str()) != ARCHIVE_OK) {
        std::cerr << "[archiver] Failed to open for writing: "
                  << archivePath << " — " << archive_error_string(a) << "\n";
        archive_write_free(a);
        return false;
    }

    bool ok = true;
    for (const auto& fp : filePaths) {
        if (!fs::exists(fp)) {
            std::cerr << "[archiver] Skipping (not found): " << fp << "\n";
            continue;
        }

        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, fs::path(fp).filename().string().c_str());
        archive_entry_set_size(entry, fs::file_size(fp));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);

        if (archive_write_header(a, entry) != ARCHIVE_OK) {
            std::cerr << "[archiver] header error: "
                      << archive_error_string(a) << "\n";
            archive_entry_free(entry);
            ok = false;
            continue;
        }

        std::ifstream f(fp, std::ios::binary);
        char buf[1 << 20];
        while (f) {
            f.read(buf, sizeof(buf));
            auto n = f.gcount();
            if (n > 0)
                archive_write_data(a, buf, n);
        }

        archive_entry_free(entry);
    }

    archive_write_close(a);
    archive_write_free(a);

    if (ok)
        std::cout << "[archiver] Archive created: " << archivePath << "\n";
    return ok;
}

// ═══════════════════════════════════════════════════════════
//  Extract
// ═══════════════════════════════════════════════════════════

bool archiver::unpack(const std::string& archivePath,
                      const std::string& outputDir)
{
    struct archive* a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    if (archive_read_open_filename(a, archivePath.c_str(), 1 << 20) != ARCHIVE_OK) {
        std::cerr << "[archiver] Failed to open: " << archivePath
                  << " — " << archive_error_string(a) << "\n";
        archive_read_free(a);
        return false;
    }

    fs::create_directories(outputDir);

    struct archive_entry* entry;
    int count = 0;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        std::string pathname = archive_entry_pathname(entry);
        fs::path outPath = fs::path(outputDir) / pathname;

        fs::path canonicalBase = fs::canonical(outputDir);
        fs::path canonicalOut = fs::weakly_canonical(outPath);
        if (canonicalOut.string().find(canonicalBase.string()) != 0) {
            std::cerr << "[archiver] Skipping (path traversal): " << pathname << "\n";
            archive_read_data_skip(a);
            continue;
        }

        fs::create_directories(outPath.parent_path());

        if (archive_entry_filetype(entry) == AE_IFDIR) {
            fs::create_directories(outPath);
            continue;
        }

        std::ofstream of(outPath, std::ios::binary);
        if (!of) {
            std::cerr << "[archiver] Failed to create: " << outPath << "\n";
            continue;
        }

        const void* buff;
        size_t size;
        int64_t offset;
        while (archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK) {
            of.write(static_cast<const char*>(buff), size);
        }
        of.close();
        count++;
        std::cout << "[archiver] " << pathname << "\n";
    }

    int r = archive_read_free(a);
    if (r == ARCHIVE_OK || count > 0) {
        std::cout << "[archiver] Extracted files: " << count << "\n";
        return true;
    }
    std::cerr << "[archiver] Read error: "
              << archive_error_string(a) << "\n";
    return false;
}

// ═══════════════════════════════════════════════════════════
//  List contents
// ═══════════════════════════════════════════════════════════

std::vector<Entry> archiver::listEntries(const std::string& archivePath)
{
    std::vector<Entry> result;

    struct archive* a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    if (archive_read_open_filename(a, archivePath.c_str(), 1 << 20) != ARCHIVE_OK) {
        std::cerr << "[archiver] Failed to open: "
                  << archivePath << "\n";
        archive_read_free(a);
        return result;
    }

    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        Entry e;
        e.name = archive_entry_pathname(entry);
        e.size = archive_entry_size(entry);
        e.isDir = (archive_entry_filetype(entry) == AE_IFDIR);
        result.push_back(e);
        archive_read_data_skip(a);
    }

    archive_read_free(a);
    return result;
}

// ═══════════════════════════════════════════════════════════
//  Support check
// ═══════════════════════════════════════════════════════════

bool archiver::isSupported(const std::string& archivePath)
{
    struct archive* a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    if (archive_read_open_filename(a, archivePath.c_str(), 1 << 20) != ARCHIVE_OK) {
        archive_read_free(a);
        return false;
    }

    struct archive_entry* entry;
    bool ok = (archive_read_next_header(a, &entry) == ARCHIVE_OK);
    archive_read_free(a);
    return ok;
}