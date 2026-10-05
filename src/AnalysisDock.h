// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// The file underneath the picture: where each MCU's bits are, where decoding
// went wrong, what the scans and segments are, and edits to the bytes.
#pragma once

#include <QFutureWatcher>
#include <QWidget>

#include "Bitstream.h"
#include "ImageDocument.h"

class QComboBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTreeWidget;

class AnalysisDock : public QWidget
{
    Q_OBJECT

public:
    explicit AnalysisDock(QWidget *parent = nullptr);

    // Re-reads the document. Cheap when the base bytes did not change; when
    // they did, the bitstream map is rebuilt in the background.
    void setDocument(const ImageDocument *doc);
    // The MCU (in the picture as currently rendered) to describe.
    void setCurrentMcu(int index);
    const bitstream::Map &map() const { return m_map; }

signals:
    void byteEditsRequested(const QVector<ByteEdit> &edits, const QString &description);
    // An MCU of the base file to select, in base coordinates; the window
    // translates it through the recipe's moves.
    void baseMcuRequested(int index);
    void mapChanged();

private:
    void buildBitstreamTab(QWidget *tab);
    void buildScansTab(QWidget *tab);
    void buildFileTab(QWidget *tab);
    void refreshSummary();
    void refreshScans();
    void refreshFile();
    void showMcu(int baseIndex);
    void onSearchResync();
    void onApplyManualEdit();

    const ImageDocument *m_doc = nullptr;
    QByteArray m_stream; // what the current map describes
    bitstream::Map m_map;
    QFutureWatcher<bitstream::Map> m_mapWatcher;
    QVector<qint32> m_unitSources;
    int m_currentBase = -1;

    QTabWidget *m_tabs = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_firstErrorButton = nullptr;
    QLabel *m_mcuLabel = nullptr;
    QPlainTextEdit *m_hex = nullptr;
    QPushButton *m_resyncButton = nullptr;
    QListWidget *m_resyncList = nullptr;
    QPushButton *m_applyResyncButton = nullptr;
    QVector<bitstream::Candidate> m_candidates;

    QComboBox *m_editKind = nullptr;
    QSpinBox *m_editOffset = nullptr;
    QSpinBox *m_editBit = nullptr;
    QSpinBox *m_editCount = nullptr;

    QTreeWidget *m_scans = nullptr;
    QPushButton *m_keepScansButton = nullptr;
    QTreeWidget *m_segments = nullptr;
    QLabel *m_trailerLabel = nullptr;
};
