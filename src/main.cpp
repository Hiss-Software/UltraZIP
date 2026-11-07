#include "MainWindow.hpp"
#include "uzip_format.hpp"
#include <QApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QDir>
#include <iostream>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;

// ─── String to enum mappings ───
static uzip::Method parseMethod(const QString& s) {
    QString m = s.toLower();
    if (m == "lzma2" || m == "lzma")  return uzip::Method::LZMA2;
    if (m == "zstd")                  return uzip::Method::Zstd;
    if (m == "store")                 return uzip::Method::Store;
    return uzip::Method::Auto;
}

static uzip::Level parseLevel(int v) {
    if (v <= 0) return uzip::Level::Fast;
    if (v <= 3) return uzip::Level::Fast;
    if (v <= 5) return uzip::Level::Normal;
    if (v <= 7) return uzip::Level::Ultra;
    if (v <= 9) return uzip::Level::Ultra;
    return uzip::Level::Max;
}

static uzip::Filter parseFilter(const QString& s) {
    QString f = s.toLower();
    if (f == "bcj")       return uzip::Filter::BCJ;
    if (f == "delta")     return uzip::Filter::Delta;
    if (f == "none")      return uzip::Filter::None;
    return uzip::Filter::Auto;
}

// ─── Recursive file gathering from directory ───
static std::vector<std::string> gatherFiles(const QStringList& inputs) {
    std::vector<std::string> result;
    for (const auto& input : inputs) {
        fs::path p(input.toStdString());
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            for (auto& entry : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
                if (entry.is_regular_file(ec))
                    result.push_back(entry.path().string());
            }
        } else if (fs::is_regular_file(p, ec)) {
            result.push_back(p.string());
        }
    }
    return result;
}

// ─── CLI: Compress ───
static int doCompress(const QString& archivePath, const QStringList& inputs,
                      uzip::Method method, uzip::Level level,
                      bool solid, uzip::Filter filter, bool benchmark)
{
    auto files = gatherFiles(inputs);
    if (files.empty()) {
        std::cerr << "[UltraZIP] No files to compress.\n";
        return 1;
    }

    std::cerr << "[UltraZIP] Files: " << files.size() << "\n";

    uint64_t totalOriginal = 0;
    for (const auto& f : files) {
        std::error_code ec;
        totalOriginal += fs::file_size(f, ec);
    }
    std::cerr << "[UltraZIP] Original size: "
              << (totalOriginal / (1024.0 * 1024.0)) << " MB\n";

    auto t0 = std::chrono::steady_clock::now();

    bool ok = uzip::create(archivePath.toStdString(), files, method, level, solid, filter);

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    if (!ok) {
        std::cerr << "[UltraZIP] Compression error!\n";
        return 1;
    }

    std::error_code ec;
    uint64_t compressedSize = fs::file_size(archivePath.toStdString(), ec);

    double ratio = totalOriginal > 0
        ? (100.0 * compressedSize / totalOriginal) : 0;
    double speedMBs = secs > 0
        ? (totalOriginal / (1024.0 * 1024.0) / secs) : 0;

    std::cerr << "[UltraZIP] Done in " << secs << " sec\n";
    std::cerr << "[UltraZIP] Result: "
              << (compressedSize / (1024.0 * 1024.0)) << " MB"
              << " (ratio: " << ratio << "%)\n";

    if (benchmark) {
        std::cerr << "[UltraZIP] Speed: " << speedMBs << " MB/s\n";
        std::cerr << "[UltraZIP] Saved: "
                  << ((totalOriginal - compressedSize) / (1024.0 * 1024.0))
                  << " MB\n";
    }

    return 0;
}

// ─── CLI: Extract ───
static int doExtract(const QString& archivePath, const QString& outputDir)
{
    QString outDir = outputDir;
    if (outDir.isEmpty()) {
        outDir = QFileInfo(archivePath).absolutePath() + "/"
               + QFileInfo(archivePath).completeBaseName();
    }

    QDir().mkpath(outDir);

    std::cerr << "[UltraZIP] Extracting to: " << outDir.toStdString() << "\n";

    auto t0 = std::chrono::steady_clock::now();

    bool ok = uzip::extract(archivePath.toStdString(), outDir.toStdString());

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    if (!ok) {
        std::cerr << "[UltraZIP] Extraction error!\n";
        return 1;
    }

    std::cerr << "[UltraZIP] Extracted in " << secs << " sec\n";
    return 0;
}

// ─── CLI: List files ───
static int doList(const QString& archivePath, bool verbose)
{
    auto entries = uzip::list(archivePath.toStdString());
    if (entries.empty()) {
        std::cerr << "[UltraZIP] Archive is empty or unreadable.\n";
        return 1;
    }

    QTextStream out(stdout);
    out << "UltraZIP — " << archivePath << "\n";
    out << "Files: " << entries.size() << "\n\n";

    uint64_t totalOriginal = 0;
    for (const auto& e : entries) {
        totalOriginal += e.originalSize;
    }

    if (verbose) {
        out << "Name                              Size          Offset\n";
        out << "───────────────────────────────────────────────────────────\n";
        for (const auto& e : entries) {
            out.setFieldWidth(34);
            out.setFieldAlignment(QTextStream::AlignLeft);
            out << e.name.c_str();
            out.setFieldWidth(14);
            out.setFieldAlignment(QTextStream::AlignRight);
            out << e.originalSize;
            out.setFieldWidth(14);
            out << e.offsetInSolid;
            out.setFieldWidth(0);
            out << "\n";
        }
        out << "\n";
    } else {
        for (const auto& e : entries) {
            out << e.name.c_str() << "\n";
        }
    }

    out << "───────────────────────────────────────────────────────────\n";
    out << "Total: " << totalOriginal << " bytes ("
        << (totalOriginal / (1024.0 * 1024.0)) << " MB)\n";

    return 0;
}

// ─── CLI: Integrity test ───
static int doTest(const QString& archivePath)
{
    std::cerr << "[UltraZIP] Integrity test: " << archivePath.toStdString() << "\n";

    auto entries = uzip::list(archivePath.toStdString());
    if (entries.empty()) {
        std::cerr << "[UltraZIP] Archive is empty or unreadable!\n";
        return 1;
    }

    std::cerr << "[UltraZIP] Files in archive: " << entries.size() << "\n";
    std::cerr << "[UltraZIP] Header read successfully.\n";

    // Full test: extract to temp folder and verify sizes
    QString tempDir = QDir::tempPath() + "/ultrazip_test_"
                    + QString::number(QCoreApplication::applicationPid());
    QDir().mkpath(tempDir);

    bool ok = uzip::extract(archivePath.toStdString(), tempDir.toStdString());
    if (!ok) {
        std::cerr << "[UltraZIP] TEST FAILED: extraction failed!\n";
        QDir(tempDir).removeRecursively();
        return 1;
    }

    // Verify count and sizes
    int mismatches = 0;
    for (const auto& e : entries) {
        QString filePath = tempDir + "/" + e.name.c_str();
        QFileInfo fi(filePath);
        if (!fi.exists()) {
            std::cerr << "[UltraZIP] MISSING: " << e.name << "\n";
            mismatches++;
        } else if (static_cast<uint64_t>(fi.size()) != e.originalSize) {
            std::cerr << "[UltraZIP] SIZE MISMATCH: " << e.name
                      << " (expected " << e.originalSize
                      << ", got " << fi.size() << ")\n";
            mismatches++;
        }
    }

    QDir(tempDir).removeRecursively();

    if (mismatches == 0) {
        std::cerr << "[UltraZIP] ALL FILES INTACT. Test passed.\n";
        return 0;
    } else {
        std::cerr << "[UltraZIP] TEST FAILED: " << mismatches
                  << " mismatches!\n";
        return 1;
    }
}

// ─── Print help ───
static void printHelp()
{
    QTextStream out(stdout);
    out << "UltraZIP 2.0 — Archiver\n\n";
    out << "Usage:\n";
    out << "  uzip <command> [options] archive [files...]\n\n";
    out << "Commands:\n";
    out << "  c, a, create    Compress files into an archive\n";
    out << "  x, e, extract   Extract an archive\n";
    out << "  l, list         List archive contents\n";
    out << "  t, test         Test archive integrity\n";
    out << "  (no command)    Launch GUI\n\n";
    out << "Compression options:\n";
    out << "  -m, --method <auto|lzma2|zstd|store>   Compression method (default: auto)\n";
    out << "  -l, --level <0-10>                      Compression level (default: 9)\n";
    out << "  -s, --solid                             Solid archive (default: on)\n";
    out << "      --no-solid                          Disable solid\n";
    out << "  -f, --filter <auto|bcj|delta|none>      Filter (default: auto)\n";
    out << "  -b, --benchmark                         Show speed and savings\n";
    out << "  -o, --output <file>                     Output archive name\n\n";
    out << "Extraction options:\n";
    out << "  -o, --output <directory>              Where to extract\n\n";
    out << "Listing options:\n";
    out << "  -v, --verbose                          Verbose output\n\n";
    out << "General options:\n";
    out << "  -h, --help        This help\n";
    out << "  -V, --version     Version\n\n";
    out << "Examples:\n";
    out << "  uzip c archive.uzip file1.txt file2.txt\n";
    out << "  uzip c -m zstd -l 19 archive.uzip ./data/\n";
    out << "  uzip x archive.uzip -o ./output/\n";
    out << "  uzip l -v archive.uzip\n";
    out << "  uzip t archive.uzip\n\n";
}

// ─── main ───
int main(int argc, char* argv[]) {
    // If no arguments — launch GUI
    if (argc == 1) {
        QApplication app(argc, argv);
        app.setApplicationName("UltraZIP");
        app.setApplicationVersion("2.0");
        MainWindow w;
        w.show();
        return app.exec();
    }

    // CLI mode
    QCoreApplication app(argc, argv);
    app.setApplicationName("UltraZIP");
    app.setApplicationVersion("2.0");

    // Parse first argument as command
    QString cmd = QString(argv[1]).toLower();

    // Commands that do not require the parser
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        printHelp();
        return 0;
    }
    if (cmd == "-V" || cmd == "--version" || cmd == "version") {
        QTextStream(stdout) << "UltraZIP 2.0\n";
        return 0;
    }

    // Available commands
    static const QStringList compressCmds = {"c", "a", "create", "compress"};
    static const QStringList extractCmds   = {"x", "e", "extract"};
    static const QStringList listCmds      = {"l", "list"};
    static const QStringList testCmds      = {"t", "test"};

    bool isCompress = compressCmds.contains(cmd);
    bool isExtract  = extractCmds.contains(cmd);
    bool isList     = listCmds.contains(cmd);
    bool isTest     = testCmds.contains(cmd);

    if (!isCompress && !isExtract && !isList && !isTest) {
        std::cerr << "Unknown command: " << cmd.toStdString() << "\n";
        std::cerr << "Use --help for usage information.\n";
        return 1;
    }

    // Gather remaining arguments (after command)
    QStringList args;
    for (int i = 2; i < argc; i++)
        args << QString(argv[i]);

    // ─── Options parser ───
    QCommandLineParser parser;
    parser.setApplicationDescription("UltraZIP — Archiver");

    QCommandLineOption methodOpt({"m", "method"},
        "Method: auto|lzma2|zstd|store", "method", "auto");
    QCommandLineOption levelOpt({"l", "level"},
        "Level: 0-10", "level", "9");
    QCommandLineOption outputOpt({"o", "output"},
        "Output path", "path");
    QCommandLineOption filterOpt({"f", "filter"},
        "Filter: auto|bcj|delta|none", "filter", "auto");
    QCommandLineOption solidOpt({"s", "solid"},
        "Solid archive");
    QCommandLineOption noSolidOpt("no-solid",
        "Disable solid");
    QCommandLineOption benchmarkOpt({"b", "benchmark"},
        "Show speed and savings");
    QCommandLineOption verboseOpt({"v", "verbose"},
        "Verbose output");

    parser.addOption(methodOpt);
    parser.addOption(levelOpt);
    parser.addOption(outputOpt);
    parser.addOption(filterOpt);
    parser.addOption(solidOpt);
    parser.addOption(noSolidOpt);
    parser.addOption(benchmarkOpt);
    parser.addOption(verboseOpt);
    parser.addHelpOption();
    parser.addVersionOption();

    // Parse
    if (!parser.parse(args)) {
        std::cerr << "Parse error: "
                  << parser.errorText().toStdString() << "\n";
        return 1;
    }

    if (parser.isSet("help")) {
        printHelp();
        return 0;
    }
    if (parser.isSet("version")) {
        QTextStream(stdout) << "UltraZIP 2.0\n";
        return 0;
    }

    // Positional arguments (archive + files)
    QStringList positional = parser.positionalArguments();
    if (positional.isEmpty()) {
        std::cerr << "No archive specified.\nUse --help for usage information.\n";
        return 1;
    }

    QString archivePath = positional.first();
    QStringList files = positional.mid(1);

    // ─── Execute commands ───

    if (isCompress) {
        if (files.isEmpty()) {
            std::cerr << "No files specified for compression.\n";
            return 1;
        }

        uzip::Method method = parseMethod(parser.value(methodOpt));
        uzip::Level level   = parseLevel(parser.value(levelOpt).toInt());
        uzip::Filter filter = parseFilter(parser.value(filterOpt));
        bool solid          = !parser.isSet(noSolidOpt); // default: on
        bool benchmark      = parser.isSet(benchmarkOpt);

        // If -o is specified, use it as archive name
        QString finalArchive = parser.isSet(outputOpt)
            ? parser.value(outputOpt)
            : archivePath;

        return doCompress(finalArchive, files, method, level, solid, filter, benchmark);
    }

    if (isExtract) {
        QString outDir = parser.isSet(outputOpt)
            ? parser.value(outputOpt)
            : QString();
        return doExtract(archivePath, outDir);
    }

    if (isList) {
        bool verbose = parser.isSet(verboseOpt);
        return doList(archivePath, verbose);
    }

    if (isTest) {
        return doTest(archivePath);
    }

    return 0;
}