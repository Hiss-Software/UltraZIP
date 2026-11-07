#pragma once

#include <QMainWindow>
#include <QTableWidget>
#include <QPushButton>
#include <QComboBox>
#include <QCheckBox>
#include <QProgressBar>
#include <QLabel>
#include <QStringList>

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private slots:
    void onAddFiles();
    void onAddFolder();
    void onRemoveSelected();
    void onClearAll();
    void onCreateArchive();
    void onExtractArchive();
    void onFormatChanged(int index);

private:
    void setupUI();
    void setupStylesheet();
    void refreshTable();

    QTableWidget* m_table = nullptr;
    QPushButton*  m_btnAdd = nullptr;
    QPushButton*  m_btnFolder = nullptr;
    QPushButton*  m_btnRemove = nullptr;
    QPushButton*  m_btnClear = nullptr;
    QPushButton*  m_btnCreate = nullptr;
    QPushButton*  m_btnExtract = nullptr;
    QComboBox*    m_cmbFormat = nullptr;
    QComboBox*    m_cmbMethod = nullptr;
    QComboBox*    m_cmbLevel = nullptr;
    QComboBox*    m_cmbFilter = nullptr;
    QCheckBox*    m_chkSolid = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel*       m_status = nullptr;
    QLabel*       m_stats = nullptr;

    QStringList   m_fileList;
    bool          m_busy = false;
};
