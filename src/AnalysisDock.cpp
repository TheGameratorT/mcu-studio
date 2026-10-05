// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "AnalysisDock.h"

#include <QApplication>
#include <QComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <limits>

#include "JpegStructure.h"

namespace {

constexpr int kHexBefore = 48;
constexpr int kHexAfter = 208;

QString markerName(quint8 m)
{
    switch (m) {
    case 0xD8: return QStringLiteral("SOI");
    case 0xD9: return QStringLiteral("EOI");
    case 0xDA: return QStringLiteral("SOS");
    case 0xDB: return QStringLiteral("DQT");
    case 0xC4: return QStringLiteral("DHT");
    case 0xDD: return QStringLiteral("DRI");
    case 0xFE: return QStringLiteral("COM");
    case 0xC0: return QStringLiteral("SOF0 (baseline)");
    case 0xC1: return QStringLiteral("SOF1 (extended)");
    case 0xC2: return QStringLiteral("SOF2 (progressive)");
    default:
        break;
    }
    if (m >= 0xE0 && m <= 0xEF)
        return QStringLiteral("APP%1").arg(m - 0xE0);
    if (m >= 0xD0 && m <= 0xD7)
        return QStringLiteral("RST%1").arg(m - 0xD0);
    if (m >= 0xC0 && m <= 0xCF)
        return QStringLiteral("SOF%1").arg(m - 0xC0);
    return QStringLiteral("FF%1").arg(m, 2, 16, QLatin1Char('0')).toUpper();
}

// A hex dump of `bytes` around `center`, with the byte at `center` bracketed.
QString hexDump(const QByteArray &bytes, qsizetype center, int bit)
{
    const qsizetype from = qMax<qsizetype>(0, (center - kHexBefore) & ~qsizetype(15));
    const qsizetype to = qMin(bytes.size(), center + kHexAfter);
    QString out;
    for (qsizetype row = from; row < to; row += 16) {
        out += QStringLiteral("%1  ").arg(row, 8, 16, QLatin1Char('0'));
        QString ascii;
        for (qsizetype i = row; i < row + 16; ++i) {
            if (i >= to) {
                out += QStringLiteral("    ");
                continue;
            }
            const quint8 b = quint8(bytes.at(i));
            const QString h = QStringLiteral("%1").arg(b, 2, 16, QLatin1Char('0'));
            out += i == center ? QStringLiteral("[%1]").arg(h) : QStringLiteral(" %1 ").arg(h);
            ascii += (b >= 32 && b < 127) ? QChar(b) : QChar(u'.');
        }
        out += QStringLiteral("  ") + ascii + QLatin1Char('\n');
    }
    if (center >= 0 && center < bytes.size()) {
        const quint8 b = quint8(bytes.at(center));
        QString bits;
        for (int i = 7; i >= 0; --i) {
            if (7 - i == bit)
                bits += QLatin1Char('|');
            bits += QChar(u'0' + ((b >> i) & 1));
        }
        out += QStringLiteral("\nbyte %1 = %2, MCU starts at the bar: %3\n")
                   .arg(center)
                   .arg(b, 2, 16, QLatin1Char('0'))
                   .arg(bits);
    }
    return out;
}

} // namespace

AnalysisDock::AnalysisDock(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    m_tabs = new QTabWidget(this);
    layout->addWidget(m_tabs);

    QWidget *bits = new QWidget;
    buildBitstreamTab(bits);
    m_tabs->addTab(bits, tr("Bitstream"));
    QWidget *scans = new QWidget;
    buildScansTab(scans);
    m_tabs->addTab(scans, tr("Scans"));
    QWidget *file = new QWidget;
    buildFileTab(file);
    m_tabs->addTab(file, tr("File"));

    connect(&m_searchWatcher, &QFutureWatcherBase::finished, this, [this] {
        QApplication::restoreOverrideCursor();
        m_resyncButton->setEnabled(true);
        m_candidates = m_searchWatcher.result();
        m_resyncList->clear();
        if (m_candidates.size() > 25)
            m_candidates.resize(25);
        for (const bitstream::Candidate &c : m_candidates) {
            const QString what = c.deltaBits == 0 ? tr("leave as is")
                : c.deltaBits > 0 ? (c.deltaBits % 8 == 0 && c.deltaBits >= 64
                                         ? tr("delete %n byte(s)", nullptr, c.deltaBits / 8)
                                         : tr("delete %n bit(s)", nullptr, c.deltaBits))
                                  : tr("insert %n zero bit(s)", nullptr, -c.deltaBits);
            m_resyncList->addItem(tr("%1 at byte %2  ·  %3 MCUs clean  ·  offset %4, seams %5")
                                      .arg(what)
                                      .arg(c.bitPos >> 3)
                                      .arg(c.cleanMcus)
                                      .arg(c.dcStep, 0, 'f', 1)
                                      .arg(c.edgeCost, 0, 'f', 1));
        }
        if (!m_candidates.isEmpty())
            m_resyncList->setCurrentRow(0);
    });
    connect(&m_mapWatcher, &QFutureWatcherBase::finished, this, [this] {
        m_map = m_mapWatcher.result();
        refreshSummary();
        showMcu(m_currentBase);
        emit mapChanged();
    });
}

void AnalysisDock::buildBitstreamTab(QWidget *tab)
{
    QVBoxLayout *v = new QVBoxLayout(tab);
    m_summary = new QLabel(tab);
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_summary);

    m_firstErrorButton = new QPushButton(tr("Select the first decode error"), tab);
    m_firstErrorButton->setToolTip(tr("Where the Huffman decoder first met bits it could not make "
                                      "sense of. The damage itself is usually at or a few MCUs "
                                      "before it."));
    connect(m_firstErrorButton, &QPushButton::clicked, this, [this] {
        if (m_map.firstAnomaly >= 0)
            emit baseMcuRequested(m_map.firstAnomaly);
    });
    v->addWidget(m_firstErrorButton);

    m_mcuLabel = new QLabel(tab);
    m_mcuLabel->setWordWrap(true);
    m_mcuLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_mcuLabel);

    m_hex = new QPlainTextEdit(tab);
    m_hex->setReadOnly(true);
    m_hex->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_hex->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_hex->setMinimumHeight(160);
    v->addWidget(m_hex, 1);

    m_resyncButton = new QPushButton(tr("Search for a resync at this MCU"), tab);
    m_resyncButton->setToolTip(
        tr("Tries every deletion or insertion of up to 64 bits at this MCU's first bit, every "
           "longer deletion up to 2 KiB, and every whole-byte deletion starting between here and "
           "the first decode error. Keeps the ones after which the stream decodes cleanly, and "
           "ranks them by the DC offset they leave against the rows above and by how the first "
           "MCUs' edges meet their neighbors. Selecting one previews it. Select the MCU where the "
           "damage starts first."));
    connect(m_resyncButton, &QPushButton::clicked, this, &AnalysisDock::onSearchResync);
    v->addWidget(m_resyncButton);
    m_resyncList = new QListWidget(tab);
    m_resyncList->setMaximumHeight(140);
    v->addWidget(m_resyncList);
    m_applyResyncButton = new QPushButton(tr("Apply the selected fix"), tab);
    m_applyResyncButton->setEnabled(false);
    connect(m_resyncList, &QListWidget::currentRowChanged, this, [this](int row) {
        const bool ok = row >= 0 && row < m_candidates.size();
        m_applyResyncButton->setEnabled(ok && m_candidates.at(row).deltaBits != 0);
        // Selecting a candidate shows it: the ranking narrows the search, the
        // eye settles it.
        if (ok && m_candidates.at(row).deltaBits != 0)
            emit byteEditsPreviewRequested({editFor(m_candidates.at(row))});
        else
            emit byteEditsPreviewRequested({});
    });
    connect(m_applyResyncButton, &QPushButton::clicked, this, [this] {
        const int row = m_resyncList->currentRow();
        if (row < 0 || row >= m_candidates.size() || m_candidates.at(row).deltaBits == 0)
            return;
        const ByteEdit e = editFor(m_candidates.at(row));
        emit byteEditsPreviewRequested({});
        emit byteEditsRequested({e}, tr("Resync: %1").arg(e.describe()));
    });
    v->addWidget(m_applyResyncButton);

    QFormLayout *form = new QFormLayout;
    m_editKind = new QComboBox(tab);
    m_editKind->addItem(tr("Delete bytes"), int(ByteEdit::Kind::DeleteBytes));
    m_editKind->addItem(tr("Insert zero bytes"), int(ByteEdit::Kind::InsertBytes));
    m_editKind->addItem(tr("Delete bits"), int(ByteEdit::Kind::DeleteBits));
    m_editKind->addItem(tr("Insert zero bits"), int(ByteEdit::Kind::InsertBits));
    m_editKind->addItem(tr("Flip one bit"), int(ByteEdit::Kind::FlipBit));
    m_editKind->addItem(tr("Cut the file here"), int(ByteEdit::Kind::TruncateAt));
    m_editOffset = new QSpinBox(tab);
    m_editOffset->setRange(0, std::numeric_limits<int>::max());
    m_editOffset->setGroupSeparatorShown(true);
    m_editBit = new QSpinBox(tab);
    m_editBit->setRange(0, 7);
    m_editCount = new QSpinBox(tab);
    m_editCount->setRange(1, std::numeric_limits<int>::max());
    QPushButton *apply = new QPushButton(tr("Apply edit"), tab);
    connect(apply, &QPushButton::clicked, this, &AnalysisDock::onApplyManualEdit);
    QHBoxLayout *where = new QHBoxLayout;
    where->addWidget(m_editOffset, 1);
    where->addWidget(new QLabel(tr("bit"), tab));
    where->addWidget(m_editBit);
    form->addRow(tr("Edit:"), m_editKind);
    form->addRow(tr("At byte:"), where);
    form->addRow(tr("Count:"), m_editCount);
    form->addRow(QString(), apply);
    v->addLayout(form);
}

void AnalysisDock::buildScansTab(QWidget *tab)
{
    QVBoxLayout *v = new QVBoxLayout(tab);
    QLabel *hint = new QLabel(
        tr("A progressive file builds the picture up over several scans: first the DC terms, "
           "then bands of AC terms, then refinements. Damage to a late scan spoils detail across "
           "the whole picture; keeping only the scans before it gives a softer picture with no "
           "corruption in it."),
        tab);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    v->addWidget(hint);
    m_scans = new QTreeWidget(tab);
    m_scans->setHeaderLabels({tr("#"), tr("Scan"), tr("Offset"), tr("Bytes"), tr("RSTs"), tr("State")});
    m_scans->setRootIsDecorated(false);
    v->addWidget(m_scans, 1);
    m_keepScansButton = new QPushButton(tr("Keep scans up to the selected one"), tab);
    connect(m_keepScansButton, &QPushButton::clicked, this, [this] {
        if (!m_doc)
            return;
        const int row = m_scans->indexOfTopLevelItem(m_scans->currentItem());
        const jpegfile::Structure s = jpegfile::walk(m_doc->streamBytes());
        if (row < 0 || row + 1 >= s.scans.size())
            return;
        ByteEdit e;
        e.kind = ByteEdit::Kind::TruncateAt;
        e.offset = s.scans[row + 1].sosOffset;
        emit byteEditsRequested({e}, tr("Kept the first %n scan(s)", nullptr, row + 1));
    });
    v->addWidget(m_keepScansButton);
}

void AnalysisDock::buildFileTab(QWidget *tab)
{
    QVBoxLayout *v = new QVBoxLayout(tab);
    m_trailerLabel = new QLabel(tab);
    m_trailerLabel->setWordWrap(true);
    m_trailerLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(m_trailerLabel);
    m_segments = new QTreeWidget(tab);
    m_segments->setHeaderLabels({tr("Segment"), tr("Offset"), tr("Length"), tr("Notes")});
    m_segments->setRootIsDecorated(false);
    v->addWidget(m_segments, 1);
}

void AnalysisDock::setDocument(const ImageDocument *doc)
{
    m_doc = doc;
    if (!doc || !doc->isOpen()) {
        m_stream.clear();
        m_map = bitstream::Map{};
        m_unitSources.clear();
        refreshSummary();
        refreshScans();
        refreshFile();
        return;
    }
    m_unitSources = doc->unitSources();
    if (doc->streamBytes() != m_stream) {
        m_stream = doc->streamBytes();
        m_map = bitstream::Map{};
        m_summary->setText(tr("Mapping the bitstream…"));
        const QByteArray bytes = doc->baseBytes();
        m_mapWatcher.setFuture(QtConcurrent::run([bytes] { return bitstream::map(bytes); }));
        refreshScans();
        refreshFile();
    }
    showMcu(m_currentBase);
}

void AnalysisDock::setCurrentMcu(int index)
{
    if (index < 0 || !m_doc || !m_doc->isOpen()) {
        showMcu(-1);
        return;
    }
    const int bpm = m_doc->info().blocksPerMcu;
    const qint32 unit = m_unitSources.value(qsizetype(index) * bpm, index * bpm);
    showMcu(unit < 0 ? -2 : unit / bpm);
}

void AnalysisDock::refreshSummary()
{
    if (!m_doc || !m_doc->isOpen()) {
        m_summary->setText(tr("No image loaded."));
        m_firstErrorButton->setEnabled(false);
        m_resyncButton->setEnabled(false);
        return;
    }
    if (!m_map.isValid()) {
        m_summary->setText(m_map.unsupported.isEmpty() ? tr("Mapping the bitstream…")
                                                       : m_map.unsupported);
        m_firstErrorButton->setEnabled(false);
        m_resyncButton->setEnabled(false);
        return;
    }
    QString text = tr("%n MCU(s) mapped.", nullptr, int(m_map.mcus.size()));
    if (m_map.firstAnomaly < 0) {
        text += QLatin1Char(' ') + tr("The stream decodes cleanly from end to end.");
    } else {
        const bitstream::McuRecord &first = m_map.mcus.at(m_map.firstAnomaly);
        text += QLatin1Char(' ')
            + tr("%n MCU(s) decode with errors. The first is MCU %1 (row %2, col %3), at byte %4: %5.",
                 nullptr, m_map.anomalyCount)
                  .arg(m_map.firstAnomaly)
                  .arg(m_map.firstAnomaly / qMax(1, m_map.mcusX))
                  .arg(m_map.firstAnomaly % qMax(1, m_map.mcusX))
                  .arg(first.bitPos >> 3)
                  .arg(bitstream::describe(first.anomalies));
    }
    if (m_map.restartInterval > 0)
        text += QLatin1Char(' ') + tr("Restart marker every %1 MCUs.").arg(m_map.restartInterval);
    m_summary->setText(text);
    m_firstErrorButton->setEnabled(m_map.firstAnomaly >= 0);
    m_resyncButton->setEnabled(true);
}

void AnalysisDock::showMcu(int base)
{
    m_currentBase = base;
    if (base == -2) {
        m_mcuLabel->setText(tr("This MCU was written by a paste or fill, so it has no bits in the file."));
        m_hex->clear();
        return;
    }
    if (!m_map.isValid() || base < 0 || base >= m_map.mcus.size()) {
        m_mcuLabel->setText(m_map.isValid() ? tr("Hover over or select an MCU to see its bits.") : QString());
        m_hex->clear();
        return;
    }
    const bitstream::McuRecord &r = m_map.mcus.at(base);
    m_mcuLabel->setText(tr("File MCU %1 (row %2, col %3): starts at byte %4 bit %5, %6 bits long. %7")
                            .arg(base)
                            .arg(base / qMax(1, m_map.mcusX))
                            .arg(base % qMax(1, m_map.mcusX))
                            .arg(r.bitPos >> 3)
                            .arg(r.bitPos & 7)
                            .arg(r.bits)
                            .arg(r.anomalies ? tr("Decoding problem: %1.").arg(bitstream::describe(r.anomalies))
                                             : tr("Decodes cleanly.")));
    m_hex->setPlainText(hexDump(m_stream, qsizetype(r.bitPos >> 3), int(r.bitPos & 7)));
    m_editOffset->setValue(int(qMin<qint64>(r.bitPos >> 3, std::numeric_limits<int>::max())));
    m_editBit->setValue(int(r.bitPos & 7));
}

ByteEdit AnalysisDock::editFor(const bitstream::Candidate &c) const
{
    ByteEdit e;
    e.kind = c.deltaBits > 0 ? ByteEdit::Kind::DeleteBits : ByteEdit::Kind::InsertBits;
    e.offset = c.bitPos >> 3;
    e.bit = int(c.bitPos & 7);
    e.count = std::abs(c.deltaBits);
    // Whole bytes at a byte boundary are a plain byte deletion: the same
    // edit, and one that reads the way the damage happened.
    if (e.kind == ByteEdit::Kind::DeleteBits && e.bit == 0 && e.count % 8 == 0) {
        bool stuffed = false;
        for (qint64 i = e.offset; i < e.offset + e.count / 8 + 1 && i < m_stream.size(); ++i)
            stuffed = stuffed || quint8(m_stream.at(i)) == 0xFF;
        if (!stuffed) {
            e.kind = ByteEdit::Kind::DeleteBytes;
            e.count /= 8;
        }
    }
    return e;
}

void AnalysisDock::onSearchResync()
{
    if (!m_map.isValid() || m_currentBase < 0 || m_currentBase >= m_map.mcus.size()) {
        QMessageBox::information(this, tr("Search for a resync"),
                                 tr("Select the MCU where the damage starts first."));
        return;
    }
    if (m_searchWatcher.isRunning())
        return;
    const qint64 bitPos = m_map.mcus.at(m_currentBase).bitPos;
    const QByteArray bytes = m_stream;
    QApplication::setOverrideCursor(Qt::BusyCursor);
    m_resyncButton->setEnabled(false);
    m_resyncList->clear();
    m_resyncList->addItem(tr("Searching…"));
    m_searchWatcher.setFuture(QtConcurrent::run([bytes, bitPos] { return bitstream::searchResync(bytes, bitPos); }));
}

void AnalysisDock::onApplyManualEdit()
{
    if (!m_doc || !m_doc->isOpen())
        return;
    ByteEdit e;
    e.kind = ByteEdit::Kind(m_editKind->currentData().toInt());
    e.offset = m_editOffset->value();
    e.bit = m_editBit->value();
    e.count = m_editCount->value();
    if (e.kind == ByteEdit::Kind::InsertBytes) {
        e.data = QByteArray(int(e.count), '\0');
        e.count = 0;
    }
    emit byteEditsRequested({e}, e.describe());
}

void AnalysisDock::refreshScans()
{
    m_scans->clear();
    if (!m_doc || !m_doc->isOpen()) {
        m_keepScansButton->setEnabled(false);
        return;
    }
    const jpegfile::Structure s = jpegfile::walk(m_doc->streamBytes());
    for (int i = 0; i < s.scans.size(); ++i) {
        const jpegfile::Scan &sc = s.scans[i];
        auto *item = new QTreeWidgetItem(m_scans);
        item->setText(0, QString::number(i + 1));
        item->setText(1, sc.describe());
        item->setText(2, QString::number(sc.sosOffset));
        item->setText(3, QString::number(sc.dataLength()));
        item->setText(4, QString::number(sc.restartMarkers));
        item->setText(5, sc.damaged() ? tr("damaged at byte %1").arg(sc.firstIllegal) : tr("intact"));
    }
    if (!s.problem.isEmpty() && s.imageEnd < 0) {
        auto *item = new QTreeWidgetItem(m_scans);
        item->setText(1, tr("(the walk stopped: this file %1)").arg(s.problem));
        item->setDisabled(true);
    }
    m_scans->header()->resizeSections(QHeaderView::ResizeToContents);
    m_keepScansButton->setEnabled(s.scans.size() > 1);
}

void AnalysisDock::refreshFile()
{
    m_segments->clear();
    if (!m_doc || !m_doc->isOpen()) {
        m_trailerLabel->clear();
        return;
    }
    const QByteArray &bytes = m_doc->streamBytes();
    const jpegfile::Structure s = jpegfile::walk(bytes);
    for (const jpegfile::Segment &seg : s.segments) {
        auto *item = new QTreeWidgetItem(m_segments);
        item->setText(0, markerName(seg.marker));
        item->setText(1, QString::number(seg.offset));
        item->setText(2, QString::number(seg.length));
        if (seg.dataOffset >= 0 && seg.marker >= 0xE0 && seg.marker <= 0xEF) {
            QByteArray sig = bytes.mid(seg.dataOffset, qMin<qsizetype>(seg.dataLength, 12));
            sig.replace('\0', ' ');
            item->setText(3, QString::fromLatin1(sig).trimmed());
        }
    }
    QString t;
    const jpegfile::Trailer &tr_ = m_doc->trailer();
    if (tr_.present()) {
        t = tr("After the image: %1.").arg(tr_.describe());
        t += QLatin1Char(' ')
            + (tr_.keepOnExport() ? tr("It is carried into exports unchanged.")
                                  : tr("It is not carried into exports."));
        if (tr_.kind == jpegfile::TrailerKind::StopDjvu && tr_.offlineIdLikely)
            t += QLatin1Char(' ')
                + tr("The personal ID looks like an offline ID: Emsisoft's STOP/Djvu decryptor may "
                     "restore this file outright.");
    } else {
        t = tr("Nothing follows the image.");
    }
    if (s.progressive())
        t += QLatin1Char('\n') + tr("Progressive: %n scan(s).", nullptr, int(s.scans.size()));
    m_trailerLabel->setText(t);
    m_segments->header()->resizeSections(QHeaderView::ResizeToContents);
}
