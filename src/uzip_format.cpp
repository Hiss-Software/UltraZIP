#include "uzip_format.hpp"

#include <zstd.h>
#include <lzma.h>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <fstream>

namespace fs = std::filesystem;
using namespace uzip;

// ─── Helpers ───

template<typename T>
static void writePod(std::ofstream& out, const T& v) {
    out.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

template<typename T>
static void readPod(std::ifstream& in, T& v) {
    in.read(reinterpret_cast<char*>(&v), sizeof(T));
}

static void writeStr(std::ofstream& out, const std::string& s) {
    uint16_t len = static_cast<uint16_t>(s.size());
    writePod(out, len);
    out.write(s.data(), len);
}

static std::string readStr(std::ifstream& in) {
    uint16_t len = 0;
    readPod(in, len);
    std::string s(len, '\0');
    in.read(s.data(), len);
    return s;
}

// ─── Data type detection ───
static bool isTextData(const std::vector<uint8_t>& sample) {
    if (sample.empty()) return true;
    size_t nullCount = 0;
    for (size_t i = 0; i < sample.size(); i++) {
        if (sample[i] == 0) nullCount++;
    }
    return (nullCount * 100 < sample.size());
}

// ─── Sort files by extension ───
static void sortFilesByType(std::vector<std::string>& paths) {
    std::sort(paths.begin(), paths.end(),
        [](const std::string& a, const std::string& b) {
            fs::path pa(a), pb(b);
            std::string ea = pa.extension().string();
            std::string eb = pb.extension().string();
            std::transform(ea.begin(), ea.end(), ea.begin(),
                [](unsigned char c) { return std::tolower(c); });
            std::transform(eb.begin(), eb.end(), eb.begin(),
                [](unsigned char c) { return std::tolower(c); });
            if (ea != eb) return ea < eb;
            return pa.filename().string() < pb.filename().string();
        });
}

// ─── Adaptive windowLog for Zstd ───
static int adaptiveWindowLog(size_t totalSize) {
    if (totalSize < (1 << 20))    return 18;
    if (totalSize < (10 << 20))   return 21;
    if (totalSize < (100 << 20))  return 26;
    return 26;
}

// ─── Zstd streaming compression ───
static bool zstdCompressStream(std::ofstream& out,
                               const std::vector<std::string>& filePaths,
                               int level)
{
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    if (!cctx) return false;

    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, std::min(level, 22));

    uint64_t totalSize = 0;
    for (const auto& fp : filePaths) {
        std::error_code ec;
        totalSize += fs::file_size(fp, ec);
    }

    int wlog = adaptiveWindowLog(totalSize);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, wlog);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_ldmHashLog, 27);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_ldmMinMatch, 64);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_chainLog, wlog);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_searchLog, 10);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_targetLength, 4096);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_strategy, ZSTD_btultra2);

    const size_t CHUNK = 1 << 24;
    std::vector<uint8_t> inBuf(CHUNK);
    std::vector<uint8_t> outBuf(ZSTD_compressBound(CHUNK));

    ZSTD_inBuffer  zin  = { inBuf.data(), 0, 0 };
    ZSTD_outBuffer zout = { outBuf.data(), outBuf.size(), 0 };

    for (const auto& fp : filePaths) {
        std::ifstream f(fp, std::ios::binary);
        if (!f) continue;

        while (true) {
            f.read(reinterpret_cast<char*>(inBuf.data()), CHUNK);
            size_t bytesRead = f.gcount();
            if (bytesRead == 0) break;

            zin.src = inBuf.data();
            zin.size = bytesRead;
            zin.pos = 0;

            while (zin.pos < zin.size) {
                zout.pos = 0;
                size_t r = ZSTD_compressStream2(cctx, &zout, &zin, ZSTD_e_continue);
                if (ZSTD_isError(r)) {
                    ZSTD_freeCCtx(cctx);
                    return false;
                }
                if (zout.pos > 0)
                    out.write(reinterpret_cast<char*>(outBuf.data()), zout.pos);
            }
        }
    }

    while (true) {
        zout.pos = 0;
        size_t r = ZSTD_compressStream2(cctx, &zout, &zin, ZSTD_e_end);
        if (zout.pos > 0)
            out.write(reinterpret_cast<char*>(outBuf.data()), zout.pos);
        if (r == 0) break;
        if (ZSTD_isError(r)) {
            ZSTD_freeCCtx(cctx);
            return false;
        }
    }

    out.flush();
    ZSTD_freeCCtx(cctx);
    return true;
}

// ─── Structure for best LZMA parameters ───
struct LzmaTune {
    uint32_t lc, lp, pb;
    bool useBCJ;
    bool useDelta;
};

// ─── LZMA compression with explicit parameters ───
static bool lzmaCompressWithParams(std::ofstream& out,
                                    const std::vector<std::string>& filePaths,
                                    int level, const LzmaTune& tune)
{
    lzma_stream strm = LZMA_STREAM_INIT;

    lzma_options_lzma lzma_opt;
    uint32_t preset = (level >= 9 ? 9 : level) | LZMA_PRESET_EXTREME;
    if (lzma_lzma_preset(&lzma_opt, preset) != LZMA_OK) return false;

    uint64_t totalSize = 0;
    for (const auto& fp : filePaths) {
        std::error_code ec;
        totalSize += fs::file_size(fp, ec);
    }

    uint64_t dictTarget = std::min<uint64_t>(totalSize, 1ULL << 28);
    if (dictTarget < (1 << 16)) lzma_opt.dict_size = 1 << 16;
    else                         lzma_opt.dict_size = static_cast<uint32_t>(dictTarget);

    lzma_opt.nice_len = 273;
    lzma_opt.mode = LZMA_MODE_NORMAL;
    lzma_opt.lc = tune.lc;
    lzma_opt.lp = tune.lp;
    lzma_opt.pb = tune.pb;

    lzma_filter filters[4];
    int fc = 0;
    lzma_options_delta delta_opt;

    if (tune.useBCJ) {
        filters[fc].id = LZMA_FILTER_X86;
        filters[fc].options = nullptr;
        fc++;
    }
    if (tune.useDelta) {
        delta_opt.type = LZMA_DELTA_TYPE_BYTE;
        delta_opt.dist = 1;
        filters[fc].id = LZMA_FILTER_DELTA;
        filters[fc].options = &delta_opt;
        fc++;
    }

    filters[fc].id = LZMA_FILTER_LZMA2;
    filters[fc].options = &lzma_opt;
    fc++;
    filters[fc].id = LZMA_VLI_UNKNOWN;
    filters[fc].options = nullptr;

    lzma_ret ret = lzma_stream_encoder(&strm, filters, LZMA_CHECK_CRC64);
    if (ret != LZMA_OK) {
        if (lzma_easy_encoder(&strm, preset, LZMA_CHECK_CRC64) != LZMA_OK)
            return false;
    }

    const size_t CHUNK = 1 << 24;
    std::vector<uint8_t> inBuf(CHUNK);
    std::vector<uint8_t> outBuf(CHUNK);

    for (const auto& fp : filePaths) {
        std::ifstream f(fp, std::ios::binary);
        if (!f) continue;

        while (true) {
            f.read(reinterpret_cast<char*>(inBuf.data()), CHUNK);
            size_t bytesRead = f.gcount();
            if (bytesRead == 0) break;

            strm.next_in = inBuf.data();
            strm.avail_in = bytesRead;

            while (strm.avail_in > 0) {
                strm.next_out = outBuf.data();
                strm.avail_out = outBuf.size();
                lzma_ret r = lzma_code(&strm, LZMA_RUN);
                size_t written = outBuf.size() - strm.avail_out;
                if (written > 0)
                    out.write(reinterpret_cast<char*>(outBuf.data()), written);
                if (r != LZMA_OK) break;
            }
        }
    }

    strm.next_in = nullptr;
    strm.avail_in = 0;
    while (true) {
        strm.next_out = outBuf.data();
        strm.avail_out = outBuf.size();
        lzma_ret r = lzma_code(&strm, LZMA_FINISH);
        size_t written = outBuf.size() - strm.avail_out;
        if (written > 0)
            out.write(reinterpret_cast<char*>(outBuf.data()), written);
        if (r == LZMA_STREAM_END) break;
        if (r != LZMA_OK) break;
    }

    out.flush();
    lzma_end(&strm);
    return true;
}

// ─── Quick parameter test on a sample (level 6, no EXTREME) ───
static uint64_t testLzmaParamsOnSample(const std::vector<uint8_t>& sampleData,
                                        uint32_t lc, uint32_t lp, uint32_t pb,
                                        bool useBCJ, bool useDelta)
{
    if (sampleData.empty()) return UINT64_MAX;

    fs::path inPath = fs::temp_directory_path() / "uzip_tune_in.tmp";
    fs::path outPath = fs::temp_directory_path() / "uzip_tune_out.tmp";

    {
        std::ofstream f(inPath, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(sampleData.data()), sampleData.size());
        f.close();
    }

    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_options_lzma lzma_opt;
    // level 6, NO EXTREME — for test speed
    if (lzma_lzma_preset(&lzma_opt, 6) != LZMA_OK) {
        fs::remove(inPath);
        return UINT64_MAX;
    }

    // Dictionary = sample size, but not less than 64 KB
    if (sampleData.size() < (1 << 16))
        lzma_opt.dict_size = 1 << 16;
    else
        lzma_opt.dict_size = static_cast<uint32_t>(sampleData.size());

    lzma_opt.nice_len = 273;
    lzma_opt.mode = LZMA_MODE_NORMAL;
    lzma_opt.lc = lc;
    lzma_opt.lp = lp;
    lzma_opt.pb = pb;

    lzma_filter filters[4];
    int fc = 0;
    lzma_options_delta delta_opt;

    if (useBCJ) {
        filters[fc].id = LZMA_FILTER_X86;
        filters[fc].options = nullptr;
        fc++;
    }
    if (useDelta) {
        delta_opt.type = LZMA_DELTA_TYPE_BYTE;
        delta_opt.dist = 1;
        filters[fc].id = LZMA_FILTER_DELTA;
        filters[fc].options = &delta_opt;
        fc++;
    }

    filters[fc].id = LZMA_FILTER_LZMA2;
    filters[fc].options = &lzma_opt;
    fc++;
    filters[fc].id = LZMA_VLI_UNKNOWN;
    filters[fc].options = nullptr;

    if (lzma_stream_encoder(&strm, filters, LZMA_CHECK_CRC64) != LZMA_OK) {
        fs::remove(inPath);
        return UINT64_MAX;
    }

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);

    const size_t CHUNK = 1 << 20; // 1 MB for test
    std::vector<uint8_t> inBuf(CHUNK);
    std::vector<uint8_t> outBuf(CHUNK);

    std::ifstream in(inPath, std::ios::binary);

    while (true) {
        in.read(reinterpret_cast<char*>(inBuf.data()), CHUNK);
        size_t bytesRead = in.gcount();
        if (bytesRead == 0) break;

        strm.next_in = inBuf.data();
        strm.avail_in = bytesRead;

        while (strm.avail_in > 0) {
            strm.next_out = outBuf.data();
            strm.avail_out = outBuf.size();
            lzma_code(&strm, LZMA_RUN);
            size_t written = outBuf.size() - strm.avail_out;
            if (written > 0)
                out.write(reinterpret_cast<char*>(outBuf.data()), written);
        }
    }

    strm.next_in = nullptr;
    strm.avail_in = 0;
    while (true) {
        strm.next_out = outBuf.data();
        strm.avail_out = outBuf.size();
        lzma_ret r = lzma_code(&strm, LZMA_FINISH);
        size_t written = outBuf.size() - strm.avail_out;
        if (written > 0)
            out.write(reinterpret_cast<char*>(outBuf.data()), written);
        if (r == LZMA_STREAM_END) break;
        if (r != LZMA_OK) break;
    }

    out.flush();
    out.close();
    in.close();
    lzma_end(&strm);

    uint64_t size = 0;
    std::error_code ec;
    size = fs::file_size(outPath, ec);

    fs::remove(inPath);
    fs::remove(outPath);
    return size;
}

// ─── Multi-file sample (read from different files) ───
static std::vector<uint8_t> readMultiSample(
    const std::vector<std::string>& filePaths, size_t totalSampleSize)
{
    std::vector<uint8_t> sample;
    sample.reserve(totalSampleSize);

    if (filePaths.empty()) return sample;

    size_t filesToRead = std::min(filePaths.size(), static_cast<size_t>(8));
    size_t perFile = totalSampleSize / filesToRead;
    if (perFile < 1) perFile = 1;

    // Evenly distribute: first, middle, last + intermediate
    for (size_t i = 0; i < filesToRead; i++) {
        size_t idx = (filePaths.size() - 1) * i / (filesToRead - 1 < 1 ? 1 : filesToRead - 1);
        if (idx >= filePaths.size()) idx = filePaths.size() - 1;

        std::ifstream f(filePaths[idx], std::ios::binary);
        if (!f) continue;

        std::vector<uint8_t> buf(perFile);
        f.read(reinterpret_cast<char*>(buf.data()), perFile);
        size_t bytesRead = f.gcount();
        buf.resize(bytesRead);
        sample.insert(sample.end(), buf.begin(), buf.end());

        if (sample.size() >= totalSampleSize) break;
    }

    if (sample.size() > totalSampleSize)
        sample.resize(totalSampleSize);
    return sample;
}

// ─── Quick Zstd test on sample ───
static uint64_t testZstdOnSample(const std::vector<uint8_t>& sampleData)
{
    if (sampleData.empty()) return UINT64_MAX;

    fs::path inPath = fs::temp_directory_path() / "uzip_zstd_in.tmp";
    fs::path outPath = fs::temp_directory_path() / "uzip_zstd_out.tmp";

    {
        std::ofstream f(inPath, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(sampleData.data()), sampleData.size());
        f.close();
    }

    std::vector<std::string> oneFile = { inPath.string() };
    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    bool ok = zstdCompressStream(out, oneFile, 19);
    out.close();

    uint64_t size = 0;
    if (ok) {
        std::error_code ec;
        size = fs::file_size(outPath, ec);
    }

    fs::remove(inPath);
    fs::remove(outPath);
    return size;
}

// ─── Auto-tune: test 5 combinations, choose the best ───
static LzmaTune autoTuneLzma(const std::vector<uint8_t>& sampleData)
{
    struct Combo {
        const char* name;
        uint32_t lc, lp, pb;
        bool useBCJ, useDelta;
    };

    // 5 combinations for testing
    Combo combos[] = {
        {"text",       0, 0, 0, false, false},  // text
        {"bin-pb0",    4, 0, 0, true,  false},  // binary, pb=0 + BCJ
        {"bin-pb2",    4, 0, 2, true,  false},  // binary, pb=2 + BCJ (7-Zip style)
        {"bin-nofilt", 4, 0, 0, false, false},  // binary without BCJ
        {"delta",      0, 0, 0, false, true },  // raw data + Delta
    };

    LzmaTune best = {4, 0, 0, false, false};
    uint64_t bestSize = UINT64_MAX;

    for (const auto& c : combos) {
        uint64_t sz = testLzmaParamsOnSample(sampleData, c.lc, c.lp, c.pb,
                                              c.useBCJ, c.useDelta);
        std::cerr << "[uzip]   LZMA " << c.name << ": " << sz << "\n";

        if (sz < bestSize) {
            bestSize = sz;
            best = {c.lc, c.lp, c.pb, c.useBCJ, c.useDelta};
        }
    }

    return best;
}

// ─── Old lzmaCompressStream function (now calls the new one) ───
static bool lzmaCompressStream(std::ofstream& out,
                               const std::vector<std::string>& filePaths,
                               int level, Filter filter)
{
    // Detect data type
    std::vector<uint8_t> sample;
    if (!filePaths.empty()) {
        std::ifstream sf(filePaths[0], std::ios::binary);
        sample.resize(1 << 16);
        sf.read(reinterpret_cast<char*>(sample.data()), sample.size());
        sample.resize(sf.gcount());
    }
    bool isText = isTextData(sample);

    LzmaTune tune;
    if (filter == Filter::Auto) {
        // If Auto — auto-tuner determines it
        // (called from create(), using fallback here)
        tune = {static_cast<uint32_t>(isText ? 0 : 4), 0, 0, !isText, false};
    } else if (filter == Filter::BCJ) {
        tune = {static_cast<uint32_t>(isText ? 0 : 4), 0, 0, true, false};
    } else if (filter == Filter::Delta) {
        tune = {0, 0, 0, false, true};
    } else {
        tune = {static_cast<uint32_t>(isText ? 0 : 4), 0, 0, false, false};
    }

    return lzmaCompressWithParams(out, filePaths, level, tune);
}

// ─── Create archive (streaming) ───
bool uzip::create(const std::string& archivePath,
                  const std::vector<std::string>& filePaths,
                  Method method, Level level, bool solid, Filter filter)
{
    std::vector<std::string> sortedPaths = filePaths;
    if (solid) sortFilesByType(sortedPaths);

    uint64_t totalOriginal = 0;
    for (const auto& fp : sortedPaths) {
        std::error_code ec;
        totalOriginal += fs::file_size(fp, ec);
    }

    std::vector<FileEntry> entries;
    uint64_t offset = 0;
    for (const auto& fp : sortedPaths) {
        std::error_code ec;
        uint64_t sz = fs::file_size(fp, ec);
        FileEntry e;
        e.name = fs::path(fp).filename().string();
        e.originalSize = sz;
        e.offsetInSolid = offset;
        e.compressedSize = 0;
        offset += sz;
        entries.push_back(std::move(e));
    }

    int lvl = static_cast<int>(level);
    Method actualMethod = method;
    LzmaTune bestTune = {4, 0, 0, false, false};

    std::ofstream out(archivePath, std::ios::binary);
    if (!out) return false;

    if (method == Method::Auto) {
        // ─── Multi-file sample 8 MB ───
        const size_t SAMPLE_SIZE = 1 << 23; // 8 MB
        std::cerr << "[uzip] Auto: reading sample from " << sortedPaths.size()
                  << " files (8 MB)...\n";
        std::vector<uint8_t> sampleData = readMultiSample(sortedPaths, SAMPLE_SIZE);

        // ─── Zstd test ───
        std::cerr << "[uzip] Testing Zstd...\n";
        uint64_t zstdSize = testZstdOnSample(sampleData);
        std::cerr << "[uzip]   Zstd: " << zstdSize << "\n";

        // ─── LZMA auto-tune: 5 combinations ───
        std::cerr << "[uzip] Auto-tuning LZMA (5 combinations)...\n";
        bestTune = autoTuneLzma(sampleData);
        std::cerr << "[uzip] Best LZMA: lc=" << bestTune.lc
                  << " lp=" << bestTune.lp
                  << " pb=" << bestTune.pb
                  << " BCJ=" << (bestTune.useBCJ ? "yes" : "no")
                  << " Delta=" << (bestTune.useDelta ? "yes" : "no") << "\n";

        // Recompress sample with best params for exact size
        fs::path bestOut = fs::temp_directory_path() / "uzip_best_lzma.tmp";
        {
            // Sample is already removed, recreate it
            fs::path samplePath = fs::temp_directory_path() / "uzip_best_sample.tmp";
            {
                std::ofstream sf(samplePath, std::ios::binary | std::ios::trunc);
                sf.write(reinterpret_cast<const char*>(sampleData.data()), sampleData.size());
                sf.close();
            }
            std::vector<std::string> sf2 = { samplePath.string() };
            std::ofstream bo(bestOut, std::ios::binary | std::ios::trunc);
            lzmaCompressWithParams(bo, sf2, lvl, bestTune);
            bo.close();
            fs::remove(samplePath);
        }
        uint64_t lzmaSize = 0;
        {
            std::error_code ec;
            lzmaSize = fs::file_size(bestOut, ec);
        }
        fs::remove(bestOut);
        std::cerr << "[uzip]   LZMA2: " << lzmaSize << "\n";

        uint64_t sampleOriginal = sampleData.size();

        if (zstdSize >= sampleOriginal && lzmaSize >= sampleOriginal) {
            actualMethod = Method::Store;
            std::cerr << "[uzip] Data not compressible — Store\n";
        } else if (zstdSize <= lzmaSize) {
            actualMethod = Method::Zstd;
            std::cerr << "[uzip] Selected Zstd\n";
        } else {
            actualMethod = Method::LZMA2;
            std::cerr << "[uzip] Selected LZMA2\n";
        }
    }

    Header hdr{};
    std::memcpy(hdr.magic, Header::MAGIC_V2, 9);
    hdr.version   = Header::VERSION;
    hdr.method    = static_cast<uint8_t>(actualMethod);
    hdr.level     = static_cast<uint8_t>(level);
    hdr.solid     = solid ? 1 : 0;
    hdr.filter    = static_cast<uint8_t>(filter);
    hdr.fileCount = static_cast<uint32_t>(entries.size());
    hdr.totalOriginal  = totalOriginal;
    hdr.totalCompressed = 0;

    writePod(out, hdr);
    for (const auto& e : entries) {
        writeStr(out, e.name);
        writePod(out, e.originalSize);
        writePod(out, e.offsetInSolid);
        writePod(out, e.compressedSize);
    }

    out.flush();
    auto dataStart = out.tellp();

    bool ok = false;
    if (actualMethod == Method::Zstd) {
        ok = zstdCompressStream(out, sortedPaths, lvl);
    } else if (actualMethod == Method::LZMA2) {
        // Use best params from auto-tune (or fallback)
        if (method == Method::Auto) {
            ok = lzmaCompressWithParams(out, sortedPaths, lvl, bestTune);
        } else {
            ok = lzmaCompressStream(out, sortedPaths, lvl, filter);
        }
    } else {
        ok = true;
        std::vector<char> buf(1 << 24);
        for (const auto& fp : sortedPaths) {
            std::ifstream f(fp, std::ios::binary);
            while (f) {
                f.read(buf.data(), buf.size());
                out.write(buf.data(), f.gcount());
            }
        }
        out.flush();
    }

    if (!ok) {
        std::cerr << "[uzip] CRITICAL: Compression failed! Deleting corrupted archive.\n";
        out.close();
        fs::remove(archivePath);
        return false;
    }

    auto dataEnd = out.tellp();
    uint64_t totalCompressed = static_cast<uint64_t>(dataEnd - dataStart);

    out.seekp(0);
    hdr.totalCompressed = totalCompressed;
    writePod(out, hdr);

    out.close();

    double ratio = totalOriginal > 0
        ? (100.0 * totalCompressed / totalOriginal) : 0;
    std::cerr << "[uzip] " << totalOriginal << " -> "
              << totalCompressed << " bytes"
              << " (ratio: " << ratio << "%)\n";
    return true;
}

// ─── Extract (streaming) ───
bool uzip::extract(const std::string& archivePath,
                   const std::string& outputDir)
{
    std::ifstream in(archivePath, std::ios::binary);
    if (!in) return false;

    Header hdr;
    readPod(in, hdr);

    if (std::memcmp(hdr.magic, "ULTRAZIP", 8) != 0) return false;
    uint8_t ver = hdr.magic[8];
    if (ver != 1 && ver != 2) return false;

    std::vector<FileEntry> entries(hdr.fileCount);
    for (uint32_t i = 0; i < hdr.fileCount; i++) {
        entries[i].name = readStr(in);
        readPod(in, entries[i].originalSize);
        readPod(in, entries[i].offsetInSolid);
        readPod(in, entries[i].compressedSize);
    }

    auto dataStart = in.tellg();

    Method method = static_cast<Method>(hdr.method);
    fs::create_directories(outputDir);

    if (hdr.solid) {
        if (method == Method::Zstd) {
            ZSTD_DCtx* dctx = ZSTD_createDCtx();
            const size_t CHUNK = 1 << 24;
            std::vector<uint8_t> inBuf(CHUNK);
            std::vector<uint8_t> outBuf(CHUNK);
            ZSTD_inBuffer  zin  = { inBuf.data(), 0, 0 };
            ZSTD_outBuffer zout = { outBuf.data(), outBuf.size(), 0 };

            size_t curFile = 0;
            uint64_t curFileWritten = 0;
            std::ofstream fout;
            fs::path curPath;

            auto flushOutput = [&](size_t bytes) {
                size_t pos = 0;
                while (pos < bytes && curFile < entries.size()) {
                    if (!fout.is_open()) {
                        curPath = fs::path(outputDir) / entries[curFile].name;
                        fout.open(curPath, std::ios::binary);
                        curFileWritten = 0;
                    }
                    uint64_t remaining = entries[curFile].originalSize - curFileWritten;
                    size_t toWrite = std::min((size_t)remaining, bytes - pos);
                    fout.write(reinterpret_cast<char*>(outBuf.data() + pos), toWrite);
                    curFileWritten += toWrite;
                    pos += toWrite;
                    if (curFileWritten >= entries[curFile].originalSize) {
                        fout.close();
                        curFile++;
                    }
                }
            };

            while (true) {
                in.read(reinterpret_cast<char*>(inBuf.data()), CHUNK);
                zin.src = inBuf.data();
                zin.size = in.gcount();
                zin.pos = 0;

                while (zin.pos < zin.size) {
                    zout.pos = 0;
                    size_t r = ZSTD_decompressStream(dctx, &zout, &zin);
                    flushOutput(zout.pos);
                    if (ZSTD_isError(r)) break;
                    if (r == 0) break;
                }
                if (in.gcount() == 0) {
                    while (true) {
                        zout.pos = 0;
                        size_t r = ZSTD_decompressStream(dctx, &zout, &zin);
                        flushOutput(zout.pos);
                        if (r == 0) break;
                        if (ZSTD_isError(r)) break;
                    }
                    break;
                }
            }
            ZSTD_freeDCtx(dctx);
        } else if (method == Method::LZMA2) {
            lzma_stream strm = LZMA_STREAM_INIT;
            if (lzma_stream_decoder(&strm, UINT64_MAX, 0) != LZMA_OK)
                return false;

            const size_t CHUNK = 1 << 24;
            std::vector<uint8_t> inBuf(CHUNK);
            std::vector<uint8_t> outBuf(CHUNK);

            size_t curFile = 0;
            uint64_t curFileWritten = 0;
            std::ofstream fout;

            auto flushOutput = [&](size_t bytes) {
                size_t pos = 0;
                while (pos < bytes && curFile < entries.size()) {
                    if (!fout.is_open()) {
                        fs::path p = fs::path(outputDir) / entries[curFile].name;
                        fout.open(p, std::ios::binary);
                        curFileWritten = 0;
                    }
                    uint64_t remaining = entries[curFile].originalSize - curFileWritten;
                    size_t toWrite = std::min((size_t)remaining, bytes - pos);
                    fout.write(reinterpret_cast<char*>(outBuf.data() + pos), toWrite);
                    curFileWritten += toWrite;
                    pos += toWrite;
                    if (curFileWritten >= entries[curFile].originalSize) {
                        fout.close();
                        curFile++;
                    }
                }
            };

            while (true) {
                in.read(reinterpret_cast<char*>(inBuf.data()), CHUNK);
                size_t bytesRead = in.gcount();
                if (bytesRead == 0) break;

                strm.next_in = inBuf.data();
                strm.avail_in = bytesRead;

                while (strm.avail_in > 0) {
                    strm.next_out = outBuf.data();
                    strm.avail_out = outBuf.size();
                    lzma_code(&strm, LZMA_RUN);
                    size_t written = outBuf.size() - strm.avail_out;
                    flushOutput(written);
                }
            }

            strm.next_in = nullptr;
            strm.avail_in = 0;
            while (true) {
                strm.next_out = outBuf.data();
                strm.avail_out = outBuf.size();
                lzma_ret r = lzma_code(&strm, LZMA_FINISH);
                size_t written = outBuf.size() - strm.avail_out;
                flushOutput(written);
                if (r == LZMA_STREAM_END) break;
                if (r != LZMA_OK) break;
            }
            lzma_end(&strm);
        } else {
            size_t curFile = 0;
            uint64_t curFileWritten = 0;
            std::ofstream fout;

            const size_t CHUNK = 1 << 24;
            std::vector<uint8_t> buf(CHUNK);

            while (curFile < entries.size()) {
                if (!fout.is_open()) {
                    fs::path p = fs::path(outputDir) / entries[curFile].name;
                    fout.open(p, std::ios::binary);
                    curFileWritten = 0;
                }
                uint64_t remaining = entries[curFile].originalSize - curFileWritten;
                size_t toRead = std::min((size_t)remaining, CHUNK);
                in.read(reinterpret_cast<char*>(buf.data()), toRead);
                size_t bytesRead = in.gcount();
                if (bytesRead == 0) break;
                fout.write(reinterpret_cast<char*>(buf.data()), bytesRead);
                curFileWritten += bytesRead;
                if (curFileWritten >= entries[curFile].originalSize) {
                    fout.close();
                    curFile++;
                }
            }
        }
    } else {
        for (const auto& e : entries) {
            in.seekg(dataStart);
            in.seekg(static_cast<std::streamoff>(e.offsetInSolid));

            std::vector<uint8_t> compressed(e.compressedSize);
            in.read(reinterpret_cast<char*>(compressed.data()), e.compressedSize);

            std::vector<uint8_t> data;
            if (method == Method::Zstd) {
                unsigned long long fsz = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
                data.resize(fsz);
                ZSTD_decompress(data.data(), fsz, compressed.data(), compressed.size());
            } else if (method == Method::LZMA2) {
                lzma_stream strm = LZMA_STREAM_INIT;
                lzma_stream_decoder(&strm, UINT64_MAX, 0);
                data.resize(e.originalSize);
                strm.next_in = compressed.data();
                strm.avail_in = compressed.size();
                strm.next_out = data.data();
                strm.avail_out = data.size();
                lzma_code(&strm, LZMA_FINISH);
                lzma_end(&strm);
            } else {
                data = compressed;
            }

            fs::path outPath = fs::path(outputDir) / e.name;
            std::ofstream of(outPath, std::ios::binary);
            of.write(reinterpret_cast<const char*>(data.data()), data.size());
        }
    }
    return true;
}

// ─── List ───
std::vector<FileEntry> uzip::list(const std::string& archivePath)
{
    std::vector<FileEntry> result;
    std::ifstream in(archivePath, std::ios::binary);
    if (!in) return result;

    Header hdr;
    readPod(in, hdr);

    if (std::memcmp(hdr.magic, "ULTRAZIP", 8) != 0) return result;

    result.resize(hdr.fileCount);
    for (uint32_t i = 0; i < hdr.fileCount; i++) {
        result[i].name = readStr(in);
        readPod(in, result[i].originalSize);
        readPod(in, result[i].offsetInSolid);
        readPod(in, result[i].compressedSize);
    }
    return result;
}