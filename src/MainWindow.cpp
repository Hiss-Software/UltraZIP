#include "MainWindow.hpp"
#include "uzip_format.hpp"
#include "archiver.hpp"

#include <QFileDialog>
#include <QMessageBox>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QFileInfo>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <chrono>
#include <thread>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setAcceptDrops(true);
    setupUI();
    setWindowTitle("UltraZIP");
    resize(800, 560);
    setMinimumSize(600, 400);
}

void MainWindow::setupUI() {
    auto* central = new QWidget;
    auto* mainLayout = new QVBoxLayout(central);
    mainLayout->setSpacing(6);
    mainLayout->setContentsMargins(8, 8, 8, 8);

    // ── File buttons ──
    auto* fileBtnLayout = new QHBoxLayout;
    fileBtnLayout->setSpacing(6);
    m_btnAdd = new QPushButton("Add files");
    m_btnFolder = new QPushButton("Add folder");
    m_btnRemove = new QPushButton("Remove");
    m_btnClear = new QPushButton("Clear");
    fileBtnLayout->addWidget(m_btnAdd);
    fileBtnLayout->addWidget(m_btnFolder);
    fileBtnLayout->addStretch();
    fileBtnLayout->addWidget(m_btnRemove);
    fileBtnLayout->addWidget(m_btnClear);
    mainLayout->addLayout(fileBtnLayout);

    // ── Table ──
    m_table = new QTableWidget(0, 4);
    m_table->setHorizontalHeaderLabels({"Name", "Size", "Type", "Path"});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    mainLayout->addWidget(m_table, 1);

    // ── Settings ──
    auto* settingsGroup = new QGroupBox("Settings");
    auto* sLayout = new QGridLayout(settingsGroup);
    sLayout->setSpacing(8);

    sLayout->addWidget(new QLabel("Format:"), 0, 0);
    m_cmbFormat = new QComboBox;
    m_cmbFormat->addItems({
        ".uzip", ".zip", ".7z", ".tar", ".tar.gz", ".tar.xz",
        ".tar.bz2", ".tar.zst", ".tar.lz4", ".tar.lzo", ".tar.Z",
        ".gz", ".xz", ".bz2", ".zst", ".lz4", ".lzo", ".Z",
        ".lha", ".lzh", ".cab", ".ar", ".cpio", ".iso", ".xar", ".warc"
    });
    sLayout->addWidget(m_cmbFormat, 0, 1);

    sLayout->addWidget(new QLabel("Engine:"), 0, 2);
    m_cmbMethod = new QComboBox;
    m_cmbMethod->addItems({"Auto", "Zstd", "LZMA2"});
    sLayout->addWidget(m_cmbMethod, 0, 3);

    sLayout->addWidget(new QLabel("Level:"), 1, 0);
    m_cmbLevel = new QComboBox;
    m_cmbLevel->addItems({"Fast (3)", "Normal (5)", "Max (19)", "Ultra (22)"});
    m_cmbLevel->setCurrentIndex(3);
    sLayout->addWidget(m_cmbLevel, 1, 1);

    sLayout->addWidget(new QLabel("Filter:"), 1, 2);
    m_cmbFilter = new QComboBox;
    m_cmbFilter->addItems({"Auto", "None", "BCJ", "Delta"});
    sLayout->addWidget(m_cmbFilter, 1, 3);

    m_chkSolid = new QCheckBox("Solid");
    m_chkSolid->setChecked(true);
    sLayout->addWidget(m_chkSolid, 2, 0, 1, 4);

    mainLayout->addWidget(settingsGroup);

    // ── Action buttons ──
    auto* actionLayout = new QHBoxLayout;
    actionLayout->setSpacing(8);
    m_btnCreate = new QPushButton("Create Archive");
    m_btnExtract = new QPushButton("Extract Archive");
    actionLayout->addWidget(m_btnCreate, 1);
    actionLayout->addWidget(m_btnExtract, 1);
    mainLayout->addLayout(actionLayout);

    // ── Progress ──
    m_progress = new QProgressBar;
    m_progress->setValue(0);
    mainLayout->addWidget(m_progress);

    // ── Status ──
    m_status = new QLabel("Ready");
    mainLayout->addWidget(m_status);

    m_stats = new QLabel("");
    mainLayout->addWidget(m_stats);

    setCentralWidget(central);

    connect(m_btnAdd,     &QPushButton::clicked, this, &MainWindow::onAddFiles);
    connect(m_btnFolder,  &QPushButton::clicked, this, &MainWindow::onAddFolder);
    connect(m_btnRemove,  &QPushButton::clicked, this, &MainWindow::onRemoveSelected);
    connect(m_btnClear,   &QPushButton::clicked, this, &MainWindow::onClearAll);
    connect(m_btnCreate,  &QPushButton::clicked, this, &MainWindow::onCreateArchive);
    connect(m_btnExtract, &QPushButton::clicked, this, &MainWindow::onExtractArchive);
    connect(m_cmbFormat, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onFormatChanged);
    onFormatChanged(0);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
    for (const auto& url : e->mimeData()->urls()) {
        QString path = url.toLocalFile();
        QFileInfo fi(path);
        if (fi.isDir()) {
            QDir d(path);
            for (const auto& entry : d.entryInfoList(QDir::Files | QDir::NoSymLinks))
                m_fileList << entry.absoluteFilePath();
        } else if (fi.isFile()) {
            m_fileList << path;
        }
    }
    refreshTable();
}

void MainWindow::onFormatChanged(int) {
    bool isUzip = m_cmbFormat->currentText().startsWith(".uzip");
    m_cmbMethod->setEnabled(isUzip);
    m_cmbFilter->setEnabled(isUzip);
    m_chkSolid->setEnabled(isUzip);
}

static QString formatSize(qint64 bytes) {
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    if (bytes < (1 << 20))
        return QString("%1 KB").arg(static_cast<double>(bytes) / 1024.0, 0, 'f', 1);
    if (bytes < (1LL << 30))
        return QString("%1 MB").arg(static_cast<double>(bytes) / (1 << 20), 0, 'f', 1);
    return QString("%1 GB").arg(static_cast<double>(bytes) / (1LL << 30), 0, 'f', 2);
}

void MainWindow::refreshTable() {
    m_table->setRowCount(0);
    qint64 totalSize = 0;
    for (const auto& fp : m_fileList) {
        QFileInfo fi(fp);
        totalSize += fi.size();
        int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, 0, new QTableWidgetItem(fi.fileName()));
        m_table->setItem(row, 1, new QTableWidgetItem(formatSize(fi.size())));
        m_table->setItem(row, 2, new QTableWidgetItem(fi.suffix().isEmpty()
            ? "" : fi.suffix().toUpper()));
        m_table->setItem(row, 3, new QTableWidgetItem(fi.absolutePath()));
    }
    m_stats->setText(QString("Files: %1 · Total size: %2")
        .arg(m_fileList.size()).arg(formatSize(totalSize)));
}

void MainWindow::onAddFiles() {
    auto files = QFileDialog::getOpenFileNames(this, "Select Files");
    for (const auto& f : files)
        if (!m_fileList.contains(f))
            m_fileList << f;
    refreshTable();
}

void MainWindow::onAddFolder() {
    auto dir = QFileDialog::getExistingDirectory(this, "Select Folder");
    if (dir.isEmpty()) return;
    QDir d(dir);
    auto entries = d.entryInfoList(QDir::Files | QDir::NoSymLinks);
    for (const auto& e : entries)
        if (!m_fileList.contains(e.absoluteFilePath()))
            m_fileList << e.absoluteFilePath();
    refreshTable();
}

void MainWindow::onRemoveSelected() {
    auto selected = m_table->selectionModel()->selectedRows();
    QSet<int> rows;
    for (const auto& idx : selected)
        rows.insert(idx.row());
    QList<int> sorted = rows.values();
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    for (int r : sorted) {
        m_fileList.removeAt(r);
        m_table->removeRow(r);
    }
    qint64 totalSize = 0;
    for (const auto& fp : m_fileList) {
        QFileInfo fi(fp);
        totalSize += fi.size();
    }
    m_stats->setText(QString("Files: %1 · Total size: %2")
        .arg(m_fileList.size()).arg(formatSize(totalSize)));
}

void MainWindow::onClearAll() {
    m_fileList.clear();
    m_table->setRowCount(0);
    m_stats->setText("");
}

void MainWindow::onCreateArchive() {
    if (m_fileList.isEmpty()) {
        QMessageBox::warning(this, "UltraZIP", "Please add files first.");
        return;
    }

    QString ext = m_cmbFormat->currentText();

    QString filterStr;
    if (ext == ".lha" || ext == ".lzh") {
        filterStr = "LHA/LZH (*.lha *.lzh)";
    } else if (ext == ".tar") {
        filterStr = "TAR (*.tar)";
    } else {
        filterStr = QString("Archive (*%1)").arg(ext);
    }

    QString path = QFileDialog::getSaveFileName(this,
        "Save Archive", QString(), filterStr);
    if (path.isEmpty()) return;

    if (!path.endsWith(ext, Qt::CaseInsensitive) &&
        !(ext == ".lha" && path.endsWith(".lzh", Qt::CaseInsensitive))) {
        if (ext == ".lha" && !path.endsWith(".lzh") && !path.endsWith(".lha"))
            path += ".lha";
        else if (!ext.startsWith(".lha"))
            path += ext;
    }

    m_busy = true;
    m_progress->setValue(0);
    m_progress->setFormat("Compressing... %p%");
    m_btnCreate->setEnabled(false);
    m_btnExtract->setEnabled(false);
    m_status->setText("Compressing...");
    m_stats->setText("");

    std::vector<std::string> files;
    for (const auto& f : m_fileList)
        files.push_back(f.toStdString());

    if (ext == ".uzip") {
        int methodIdx = m_cmbMethod->currentIndex();
        uzip::Method method = (methodIdx == 0) ? uzip::Method::Auto :
                             (methodIdx == 1) ? uzip::Method::Zstd :
                                                 uzip::Method::LZMA2;

        int levelIdx = m_cmbLevel->currentIndex();
        uzip::Level level = uzip::Level::Ultra;
        if (levelIdx == 0)      level = uzip::Level::Fast;
        else if (levelIdx == 1) level = uzip::Level::Normal;
        else if (levelIdx == 2) level = uzip::Level::Max;
        else                    level = uzip::Level::Ultra;

        int filterIdx = m_cmbFilter->currentIndex();
        uzip::Filter filter = (filterIdx == 0) ? uzip::Filter::Auto :
                               (filterIdx == 1) ? uzip::Filter::None :
                               (filterIdx == 2) ? uzip::Filter::BCJ :
                                                   uzip::Filter::Delta;

        bool solid = m_chkSolid->isChecked();

        std::thread([this, path, files, method, level, solid, filter]() {
            auto start = std::chrono::high_resolution_clock::now();

            QMetaObject::invokeMethod(this, [this]() {
                m_progress->setValue(10);
            });

            bool ok = uzip::create(path.toStdString(), files,
                                   method, level, solid, filter);

            auto end = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          end - start).count();

            QFile af(path);
            qint64 arcSize = 0;
            if (af.exists()) arcSize = af.size();

            qint64 origSize = 0;
            for (const auto& f : files)
                origSize += QFile(QString::fromStdString(f)).size();

            QMetaObject::invokeMethod(this, [this, ok, ms, arcSize, origSize]() {
                m_progress->setValue(100);
                m_progress->setFormat("%p%");
                m_busy = false;
                m_btnCreate->setEnabled(true);
                m_btnExtract->setEnabled(true);

                if (ok) {
                    double ratio = origSize > 0
                        ? (100.0 * arcSize / origSize) : 0;
                    m_status->setText(QString("Done in %1 ms").arg(ms));
                    m_stats->setText(QString(
                        "Original: %1 -> Archive: %2 · %3%")
                        .arg(formatSize(origSize))
                        .arg(formatSize(arcSize))
                        .arg(ratio, 0, 'f', 1));
                    QMessageBox::information(this, "UltraZIP",
                        QString("Archive created.\nTime: %1 ms\nSize: %2")
                            .arg(ms).arg(formatSize(arcSize)));
                } else {
                    m_status->setText("Error");
                    QMessageBox::critical(this, "UltraZIP",
                        "Failed to create archive.");
                }
            });
        }).detach();
    } else {
        std::thread([this, path, files]() {
            auto start = std::chrono::high_resolution_clock::now();

            bool ok = archiver::pack(path.toStdString(), files);

            auto end = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          end - start).count();

            QFile af(path);
            qint64 arcSize = 0;
            if (af.exists()) arcSize = af.size();

            qint64 origSize = 0;
            for (const auto& f : files)
                origSize += QFile(QString::fromStdString(f)).size();

            QMetaObject::invokeMethod(this, [this, ok, ms, arcSize, origSize]() {
                m_progress->setValue(100);
                m_progress->setFormat("%p%");
                m_busy = false;
                m_btnCreate->setEnabled(true);
                m_btnExtract->setEnabled(true);

                if (ok) {
                    double ratio = origSize > 0
                        ? (100.0 * arcSize / origSize) : 0;
                    m_status->setText(QString("Done in %1 ms").arg(ms));
                    m_stats->setText(QString(
                        "Original: %1 -> Archive: %2 · %3%")
                        .arg(formatSize(origSize))
                        .arg(formatSize(arcSize))
                        .arg(ratio, 0, 'f', 1));
                    QMessageBox::information(this, "UltraZIP",
                        QString("Archive created.\nTime: %1 ms\nSize: %2")
                            .arg(ms).arg(formatSize(arcSize)));
                } else {
                    m_status->setText("Error");
                    QMessageBox::critical(this, "UltraZIP",
                        "Archive creation error.");
                }
            });
        }).detach();
    }
}

void MainWindow::onExtractArchive() {
    QString path = QFileDialog::getOpenFileName(this, "Open Archive",
        QString(),
        "All Archives (*.uzip *.zip *.7z *.tar *.tar.gz *.tar.xz *.tar.bz2 "
        "*.tar.zst *.tar.lz4 *.tar.lzo *.tar.Z *.gz *.xz *.bz2 *.zst "
        "*.lz4 *.lzo *.Z *.rar *.lha *.lzh *.cab *.ar *.cpio *.iso "
        "*.xar *.warc *.deb);;"
        "UltraZIP (*.uzip);;"
        "ZIP (*.zip *.jar *.apk);;"
        "7-Zip (*.7z);;"
        "TAR (*.tar *.tar.gz *.tar.xz *.tar.bz2 *.tar.zst);;"
        "RAR (*.rar);;"
        "LHA/LZH (*.lha *.lzh);;"
        "CAB (*.cab);;"
        "All Files (*.*)");

    if (path.isEmpty()) return;

    QString outDir = QFileDialog::getExistingDirectory(this,
        "Extract to...");
    if (outDir.isEmpty()) return;

    m_busy = true;
    m_progress->setValue(0);
    m_progress->setFormat("Extracting... %p%");
    m_btnCreate->setEnabled(false);
    m_btnExtract->setEnabled(false);
    m_status->setText("Extracting...");
    m_stats->setText("");

    std::thread([this, path, outDir]() {
        auto start = std::chrono::high_resolution_clock::now();

        bool ok;
        if (path.endsWith(".uzip", Qt::CaseInsensitive)) {
            ok = uzip::extract(path.toStdString(), outDir.toStdString());
        } else {
            ok = archiver::unpack(path.toStdString(), outDir.toStdString());
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      end - start).count();

        QMetaObject::invokeMethod(this, [this, ok, ms]() {
            m_progress->setValue(100);
            m_progress->setFormat("%p%");
            m_busy = false;
            m_btnCreate->setEnabled(true);
            m_btnExtract->setEnabled(true);

            if (ok) {
                m_status->setText(QString("Extracted in %1 ms").arg(ms));
                QMessageBox::information(this, "UltraZIP",
                    QString("Extracted.\nTime: %1 ms").arg(ms));
            } else {
                m_status->setText("Error");
                QMessageBox::critical(this, "UltraZIP", "Extraction error.");
            }
        });
    }).detach();
}