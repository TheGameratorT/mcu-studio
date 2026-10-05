// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "DonorHeaderDialog.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QInputDialog>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressDialog>
#include <QtConcurrent>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>

#include <limits>

#include "DonorSearch.h"
#include "JpegRepair.h"
#include "JpegStructure.h"
#include "PlatformStyle.h"

namespace {

// Long enough that holding the spin box's arrow key down does not queue a
// decode per step, short enough that a click feels answered.
constexpr int kPreviewDebounceMs = 160;

constexpr int kPreviewWidth = 520;
constexpr int kPreviewHeight = 360;

// Bounds on the hunt for a sibling: enough to find one in a camera roll,
// few enough that opening the dialog over a folder of thousands does not
// stall, and small enough that a stray huge file is not read for nothing.
constexpr int kMaxSuggestionTries = 12;
constexpr qint64 kMaxSuggestionBytes = 64 * 1024 * 1024;

QString humanSize(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes);
}

} // namespace

DonorHeaderDialog::DonorHeaderDialog(const QString &brokenPath, const QByteArray &brokenBytes,
                                     QWidget *parent)
    : QDialog(parent)
    , m_broken(brokenBytes)
    , m_brokenLayout(donor::scan(brokenBytes))
{
    setWindowTitle(tr("Open with a Donor Header"));
    buildUi(brokenPath);
    suggestDonorFromFolder(brokenPath);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void DonorHeaderDialog::buildUi(const QString &brokenPath)
{
    QVBoxLayout *layout = new QVBoxLayout(this);

    QLabel *intro = new QLabel(
        tr("A JPEG's tables, frame header and scan header live in front of the picture data, "
           "and nothing in the data can replace them once they are gone. A camera writes the "
           "same ones into every frame it shoots at the same settings, so another photograph "
           "from the same card (same camera, same resolution, same quality) can lend its "
           "header to this one.\n\n"
           "The picture will usually come out shifted, because there is no way to know how "
           "much data went with the header. Open it anyway: inserting and deleting MCUs slides "
           "the stream back into place."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    QFormLayout *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    const QFileInfo brokenInfo(brokenPath);
    m_brokenLabel = new QLabel(this);
    m_brokenLabel->setWordWrap(true);
    m_brokenLabel->setText(tr("%1  ·  %2\n%3")
                               .arg(brokenInfo.fileName(), humanSize(m_broken.size()),
                                    donor::describe(m_brokenLayout)));
    form->addRow(tr("Damaged file:"), m_brokenLabel);

    QHBoxLayout *donorRow = new QHBoxLayout;
    m_donorEdit = new QLineEdit(this);
    m_donorEdit->setReadOnly(true);
    m_donorEdit->setPlaceholderText(tr("No donor chosen yet"));
    QPushButton *browse = new QPushButton(tr("Browse…"), this);
    connect(browse, &QPushButton::clicked, this, &DonorHeaderDialog::onBrowseDonor);
    QPushButton *rank = new QPushButton(tr("Rank a folder…"), this);
    rank->setToolTip(tr("Splice every JPEG in a folder onto this file and rank them by how cleanly "
                        "the data decodes under their tables. The wrong tables give an error every "
                        "few bytes; the right ones hardly any."));
    connect(rank, &QPushButton::clicked, this, &DonorHeaderDialog::onRankDonors);
    donorRow->addWidget(m_donorEdit, 1);
    donorRow->addWidget(browse);
    donorRow->addWidget(rank);
    form->addRow(tr("Donor JPEG:"), donorRow);

    m_donorStatus = new QLabel(this);
    m_donorStatus->setWordWrap(true);
    form->addRow(QString(), m_donorStatus);

    m_candidateCombo = new QComboBox(this);
    m_candidateCombo->setToolTip(
        tr("Guesses at where the damaged file's surviving data begins. Try them in turn. "
           "The preview shows which one was right."));
    connect(m_candidateCombo, &QComboBox::currentIndexChanged, this,
            &DonorHeaderDialog::onCandidateChosen);
    form->addRow(tr("Data resumes:"), m_candidateCombo);

    m_offsetSpin = new QSpinBox(this);
    m_offsetSpin->setRange(0, m_broken.isEmpty()
                                  ? 0
                                  : int(qMin<qsizetype>(m_broken.size() - 1,
                                                        std::numeric_limits<int>::max())));
    m_offsetSpin->setGroupSeparatorShown(true);
    m_offsetSpin->setAccelerated(true);
    m_offsetSpin->setToolTip(tr("Byte offset into the damaged file. Nudge it when a guess is "
                                "close but the picture is not quite right."));
    connect(m_offsetSpin, &QSpinBox::valueChanged, this, &DonorHeaderDialog::onOffsetEdited);
    form->addRow(tr("Byte offset:"), m_offsetSpin);

    layout->addLayout(form);

    // STOP/Djvu leaves its signature behind; when it is there, say what it
    // means before anyone spends an afternoon on a repair.
    m_djvuLabel = new QLabel(this);
    m_djvuLabel->setWordWrap(true);
    m_djvuLabel->setStyleSheet(warningTextStyle());
    m_djvuLabel->hide();
    if (const auto footer = jpegfile::findStopDjvuFooter(m_broken)) {
        QString text = tr("This file carries the STOP/Djvu ransomware footer: its first 150 KiB were "
                          "encrypted and the rest is intact. The footer is dropped from the splice.");
        if (!footer->personalId.isEmpty()) {
            text += QLatin1Char(' ')
                + (footer->offlineIdLikely
                       ? tr("Its personal ID (%1) ends in \"t1\", the mark of an offline ID: files "
                            "encrypted with an offline key can often be decrypted outright with "
                            "Emsisoft's free STOP/Djvu decryptor, which beats any repair. Try that "
                            "first.")
                             .arg(footer->personalId)
                       : tr("Its personal ID is %1. It does not look like an offline ID, so a "
                            "decryptor is unlikely to help and repair is the way forward.")
                             .arg(footer->personalId));
        }
        m_djvuLabel->setText(text);
        m_djvuLabel->show();
    }
    layout->addWidget(m_djvuLabel);

    QGroupBox *header = new QGroupBox(tr("Header"), this);
    QFormLayout *headerForm = new QFormLayout(header);
    m_keepOwnCheck = new QCheckBox(tr("Keep this file's own tables where they survive"), header);
    m_keepOwnCheck->setChecked(true);
    m_keepOwnCheck->setToolTip(
        tr("A partly damaged header often still holds the quantization and Huffman tables this "
           "file's data was coded with. They are used table by table, and the donor fills in "
           "only what is missing."));
    m_renumberCheck = new QCheckBox(tr("Renumber restart markers to start at RST0"), header);
    m_renumberCheck->setChecked(true);
    m_renumberCheck->setToolTip(
        tr("libjpeg expects the first restart marker to be RST0 and treats any other as a lost "
           "or repeated interval. Renumbering keeps the gaps between markers, so a genuinely lost "
           "interval still shows."));
    headerForm->addRow(m_keepOwnCheck);
    headerForm->addRow(m_renumberCheck);

    QHBoxLayout *sizeRow = new QHBoxLayout;
    m_widthSpin = new QSpinBox(header);
    m_heightSpin = new QSpinBox(header);
    for (QSpinBox *spin : {m_widthSpin, m_heightSpin}) {
        spin->setRange(0, 65535);
        spin->setSpecialValueText(tr("donor's"));
        spin->setGroupSeparatorShown(true);
        connect(spin, &QSpinBox::valueChanged, this, &DonorHeaderDialog::schedulePreview);
    }
    QPushButton *swap = new QPushButton(tr("Swap"), header);
    swap->setToolTip(tr("Portrait and landscape shots from one camera share every table and differ "
                        "only in this."));
    connect(swap, &QPushButton::clicked, this, [this] {
        const int w = m_widthSpin->value() ? m_widthSpin->value() : m_donorLayout.width;
        const int h = m_heightSpin->value() ? m_heightSpin->value() : m_donorLayout.height;
        m_widthSpin->setValue(h);
        m_heightSpin->setValue(w);
    });
    QPushButton *detect = new QPushButton(tr("Detect width…"), header);
    detect->setToolTip(tr("Tries frame widths and keeps the ones under which MCU rows continue each "
                          "other. For a donor of the wrong resolution, or a file whose own frame "
                          "header is gone."));
    connect(detect, &QPushButton::clicked, this, &DonorHeaderDialog::onDetectWidth);
    sizeRow->addWidget(m_widthSpin);
    sizeRow->addWidget(new QLabel(QStringLiteral("×"), header));
    sizeRow->addWidget(m_heightSpin);
    sizeRow->addWidget(swap);
    sizeRow->addWidget(detect);
    headerForm->addRow(tr("Frame size:"), sizeRow);

    m_restartSpin = new QSpinBox(header);
    m_restartSpin->setRange(-1, 65535);
    m_restartSpin->setValue(-1);
    m_restartSpin->setSpecialValueText(tr("as in the header"));
    m_restartSpin->setToolTip(tr("MCUs between restart markers. 0 declares none."));
    connect(m_restartSpin, &QSpinBox::valueChanged, this, &DonorHeaderDialog::schedulePreview);
    headerForm->addRow(tr("Restart interval:"), m_restartSpin);
    connect(m_keepOwnCheck, &QCheckBox::toggled, this, &DonorHeaderDialog::schedulePreview);
    connect(m_renumberCheck, &QCheckBox::toggled, this, &DonorHeaderDialog::schedulePreview);
    layout->addWidget(header);

    m_preview = new QLabel(this);
    m_preview->setMinimumSize(kPreviewWidth, kPreviewHeight);
    // Ignored, so that setting a pixmap scaled to the label cannot feed back
    // into the layout and grow the label that holds it.
    m_preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setWordWrap(true);
    m_preview->setFrameShape(QFrame::StyledPanel);
    layout->addWidget(m_preview, 1);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    QPushButton *open = m_buttons->addButton(tr("Open"), QDialogButtonBox::AcceptRole);
    open->setDefault(true);
    connect(m_buttons, &QDialogButtonBox::accepted, this, [this] {
        if (m_haveResult)
            accept();
    });
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kPreviewDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &DonorHeaderDialog::rebuild);

    setPreviewMessage(tr("Choose a donor to see what comes out."));
    m_candidateCombo->setEnabled(false);
    m_offsetSpin->setEnabled(false);
    open->setEnabled(false);
}

// ---------------------------------------------------------------------------
// The donor
// ---------------------------------------------------------------------------

bool DonorHeaderDialog::setDonor(const QString &path)
{
    const QString name = QFileInfo(path).fileName();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_lastDonorError = tr("Could not read %1: %2").arg(name, file.errorString());
        return false;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    const donor::Layout layout = donor::scan(bytes);
    const QString problem = donor::donorProblem(layout);
    if (!problem.isEmpty()) {
        m_lastDonorError = tr("%1 cannot serve as a donor: it %2").arg(name, problem);
        return false;
    }

    m_donorPath = path;
    m_donorBytes = bytes;
    m_donorLayout = layout;
    m_lastDonorError.clear();

    m_donorEdit->setText(path);
    m_donorStatus->setText(donor::describe(layout));
    m_candidateCombo->setEnabled(true);
    m_offsetSpin->setEnabled(true);
    refreshCandidates();
    return true;
}

void DonorHeaderDialog::onBrowseDonor()
{
    const QString start = m_donorPath.isEmpty() ? QFileInfo(m_donorEdit->text()).absolutePath()
                                                : m_donorPath;
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose a donor JPEG"), start,
        tr("JPEG images (*.jpg *.jpeg *.JPG *.JPEG);;All files (*)"));
    if (path.isEmpty())
        return;
    if (!setDonor(path))
        m_donorStatus->setText(m_lastDonorError);
}

void DonorHeaderDialog::suggestDonorFromFolder(const QString &brokenPath)
{
    const QFileInfo brokenInfo(brokenPath);
    const QDir dir = brokenInfo.absoluteDir();
    const QStringList filters{QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
                              QStringLiteral("*.JPG"), QStringLiteral("*.JPEG")};

    int tried = 0;
    const QFileInfoList entries = dir.entryInfoList(filters, QDir::Files, QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (entry.absoluteFilePath() == brokenInfo.absoluteFilePath())
            continue;
        if (entry.size() > kMaxSuggestionBytes)
            continue;
        if (++tried > kMaxSuggestionTries)
            break;
        if (setDonor(entry.absoluteFilePath())) {
            m_donorStatus->setText(tr("%1, suggested because it sits in the same folder. "
                                      "Choose another if this shot used different settings.")
                                       .arg(donor::describe(m_donorLayout)));
            return;
        }
    }

    m_donorStatus->setText(tr("Pick a JPEG that opens, taken by the same camera at the same "
                              "resolution and quality."));
}

donor::SpliceOptions DonorHeaderDialog::spliceOptions() const
{
    donor::SpliceOptions o;
    o.keepOwnTables = m_keepOwnCheck->isChecked();
    o.renumberRestarts = m_renumberCheck->isChecked();
    o.width = m_widthSpin->value();
    o.height = m_heightSpin->value();
    o.restartInterval = m_restartSpin->value();
    return o;
}

void DonorHeaderDialog::onRankDonors()
{
    const QString start = m_donorPath.isEmpty() ? QFileInfo(m_donorEdit->text()).absolutePath()
                                                : QFileInfo(m_donorPath).absolutePath();
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Folder of candidate donors"), start);
    if (folder.isEmpty())
        return;
    QStringList paths;
    for (const QFileInfo &fi : QDir(folder).entryInfoList(
             {QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.JPG"),
              QStringLiteral("*.JPEG")},
             QDir::Files, QDir::Name)) {
        if (fi.size() <= kMaxSuggestionBytes)
            paths << fi.absoluteFilePath();
    }
    if (paths.isEmpty()) {
        QMessageBox::information(this, tr("No JPEGs"), tr("That folder has no JPEGs to try."));
        return;
    }

    QProgressDialog progress(tr("Trying %n donor(s)…", nullptr, int(paths.size())), tr("Cancel"), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    std::atomic<bool> cancelled{false};
    const QByteArray broken = m_broken;
    const qsizetype offset = m_offsetSpin->value();
    const donor::SpliceOptions options = spliceOptions();
    QFutureWatcher<QVector<donorsearch::Ranked>> watcher;
    connect(&watcher, &QFutureWatcherBase::finished, &progress, &QProgressDialog::reset);
    connect(&progress, &QProgressDialog::canceled, this, [&cancelled] { cancelled = true; });
    watcher.setFuture(QtConcurrent::run([broken, offset, paths, options, &cancelled] {
        return donorsearch::rank(broken, offset, paths, options, [&cancelled] { return cancelled.load(); });
    }));
    progress.exec();
    watcher.waitForFinished();
    const QVector<donorsearch::Ranked> ranked = watcher.result();

    QDialog chooser(this);
    chooser.setWindowTitle(tr("Donors ranked by decode health"));
    QVBoxLayout *v = new QVBoxLayout(&chooser);
    QLabel *hint = new QLabel(tr("Each donor was spliced on at byte %1 and the first part of the data "
                                 "decoded under its tables. Higher is better.")
                                  .arg(offset),
                              &chooser);
    hint->setWordWrap(true);
    v->addWidget(hint);
    QListWidget *list = new QListWidget(&chooser);
    for (const donorsearch::Ranked &r : ranked) {
        QString text = QFileInfo(r.path).fileName();
        if (r.usable()) {
            text += tr("  ·  %1%  ·  %2 of %3 MCUs clean  ·  %4×%5")
                        .arg(int(r.score * 100))
                        .arg(r.decodedMcus - r.anomalies)
                        .arg(r.decodedMcus)
                        .arg(r.width)
                        .arg(r.height);
        } else {
            text += tr("  ·  unusable: %1").arg(r.problem);
        }
        if (!r.camera.isEmpty())
            text += QStringLiteral("  ·  ") + r.camera;
        auto *item = new QListWidgetItem(text, list);
        item->setData(Qt::UserRole, r.path);
        if (!r.usable())
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
    }
    if (list->count() > 0)
        list->setCurrentRow(0);
    v->addWidget(list, 1);
    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &chooser);
    connect(buttons, &QDialogButtonBox::accepted, &chooser, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &chooser, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, &chooser, &QDialog::accept);
    v->addWidget(buttons);
    chooser.resize(720, 420);
    if (chooser.exec() != QDialog::Accepted || !list->currentItem())
        return;
    const QString chosen = list->currentItem()->data(Qt::UserRole).toString();
    if (!setDonor(chosen))
        m_donorStatus->setText(m_lastDonorError);
}

void DonorHeaderDialog::onDetectWidth()
{
    if (m_donorBytes.isEmpty())
        return;
    const int guess = std::max(m_donorLayout.width, m_donorLayout.height);
    QProgressDialog progress(tr("Trying frame widths…"), tr("Cancel"), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    std::atomic<bool> cancelled{false};
    const QByteArray donorBytes = m_donorBytes, broken = m_broken;
    const qsizetype offset = m_offsetSpin->value();
    donor::SpliceOptions options = spliceOptions();
    options.width = options.height = 0;
    QFutureWatcher<QVector<donorsearch::WidthCandidate>> watcher;
    connect(&watcher, &QFutureWatcherBase::finished, &progress, &QProgressDialog::reset);
    connect(&progress, &QProgressDialog::canceled, this, [&cancelled] { cancelled = true; });
    watcher.setFuture(QtConcurrent::run([=, &cancelled] {
        return donorsearch::detectWidth(donorBytes, broken, offset, options, 64, guess * 2,
                                        [&cancelled] { return cancelled.load(); });
    }));
    progress.exec();
    watcher.waitForFinished();
    const auto found = watcher.result();
    if (found.isEmpty()) {
        QMessageBox::information(this, tr("Detect width"), tr("No width decoded far enough to judge."));
        return;
    }
    QStringList items;
    for (int i = 0; i < found.size() && i < 12; ++i)
        items << tr("%1 × %2   (seam %3)").arg(found[i].width).arg(found[i].height).arg(found[i].cost, 0, 'f', 2);
    bool ok = false;
    const QString pick = QInputDialog::getItem(this, tr("Detect width"),
                                               tr("Widths under which MCU rows line up best:"), items,
                                               0, false, &ok);
    if (!ok)
        return;
    const int i = int(items.indexOf(pick));
    m_widthSpin->setValue(found[i].width);
    m_heightSpin->setValue(found[i].height);
}

// ---------------------------------------------------------------------------
// The splice point
// ---------------------------------------------------------------------------

void DonorHeaderDialog::refreshCandidates()
{
    m_candidates = donor::splicePoints(m_broken, m_brokenLayout, m_donorLayout.restartInterval);

    auto *model = qobject_cast<QStandardItemModel *>(m_candidateCombo->model());

    m_updatingControls = true;
    m_candidateCombo->clear();
    for (int i = 0; i < m_candidates.size(); ++i) {
        const donor::SplicePoint &point = m_candidates.at(i);
        if (point.isAvailable()) {
            m_candidateCombo->addItem(point.reason);
            continue;
        }
        // Listed but not offered, so that a resume point this file cannot
        // provide says why rather than simply not being there.
        m_candidateCombo->addItem(tr("%1: %2").arg(point.reason, point.unavailable));
        if (QStandardItem *item = model ? model->item(i) : nullptr)
            item->setEnabled(false);
        m_candidateCombo->setItemData(i, point.unavailable, Qt::ToolTipRole);
    }
    m_candidateCombo->addItem(tr("A byte offset I choose"));

    // Byte 0 always comes first and is always available, so it is the default.
    m_candidateCombo->setCurrentIndex(0);
    if (!m_candidates.isEmpty() && m_candidates.first().isAvailable())
        m_offsetSpin->setValue(int(m_candidates.first().offset));
    m_updatingControls = false;

    schedulePreview();
}

void DonorHeaderDialog::onCandidateChosen(int index)
{
    if (m_updatingControls || index < 0 || index >= m_candidates.size())
        return;
    if (!m_candidates.at(index).isAvailable()) // disabled, so only reachable by code
        return;
    m_updatingControls = true;
    m_offsetSpin->setValue(int(m_candidates.at(index).offset));
    m_updatingControls = false;
    schedulePreview();
}

void DonorHeaderDialog::onOffsetEdited()
{
    if (m_updatingControls) {
        schedulePreview();
        return;
    }
    // Keep the combo honest about what is actually being spliced.
    const qsizetype offset = m_offsetSpin->value();
    int match = m_candidateCombo->count() - 1; // the "offset I choose" row
    for (int i = 0; i < m_candidates.size(); ++i) {
        if (m_candidates.at(i).isAvailable() && m_candidates.at(i).offset == offset) {
            match = i;
            break;
        }
    }
    m_updatingControls = true;
    m_candidateCombo->setCurrentIndex(match);
    m_updatingControls = false;
    schedulePreview();
}

// ---------------------------------------------------------------------------
// Preview
// ---------------------------------------------------------------------------

void DonorHeaderDialog::schedulePreview()
{
    m_haveResult = false;
    for (QAbstractButton *button : m_buttons->buttons()) {
        if (m_buttons->buttonRole(button) == QDialogButtonBox::AcceptRole)
            button->setEnabled(false);
    }
    m_debounce->start();
}

void DonorHeaderDialog::setPreviewMessage(const QString &message)
{
    m_previewImage = QImage();
    m_preview->setPixmap(QPixmap());
    m_preview->setText(message);
}

void DonorHeaderDialog::showPreviewImage()
{
    if (m_previewImage.isNull())
        return;
    m_preview->setPixmap(QPixmap::fromImage(m_previewImage)
                             .scaled(m_preview->contentsRect().size(), Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation));
}

void DonorHeaderDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    showPreviewImage();
}

void DonorHeaderDialog::rebuild()
{
    m_haveResult = false;

    const auto enableOpen = [this](bool on) {
        for (QAbstractButton *button : m_buttons->buttons()) {
            if (m_buttons->buttonRole(button) == QDialogButtonBox::AcceptRole)
                button->setEnabled(on);
        }
    };

    if (m_donorBytes.isEmpty()) {
        setPreviewMessage(tr("Choose a donor to see what comes out."));
        m_status->clear();
        enableOpen(false);
        return;
    }

    const qsizetype offset = m_offsetSpin->value();
    QString error;
    const auto spliced = donor::splice(m_donorBytes, m_donorLayout, m_broken, offset, &error,
                                       spliceOptions());
    if (!spliced) {
        setPreviewMessage(error);
        m_status->clear();
        enableOpen(false);
        return;
    }

    const auto rgb = jr::decodeRgb(spliced->bytes, &error);
    if (!rgb || !rgb->isValid()) {
        // Wrong donor, or a splice point still sitting inside the damage --
        // which is what byte 0 gives whenever the front of the file was
        // overwritten rather than merely lost. Both are ordinary here, and
        // both are answered by trying the next candidate, so point at it
        // instead of raising an error.
        QString hint = tr("Try another resume point, or a donor from the same camera.");
        for (int i = m_candidateCombo->currentIndex() + 1; i < m_candidates.size(); ++i) {
            if (m_candidates.at(i).isAvailable()) {
                hint = tr("Try the next resume point: %1.").arg(m_candidates.at(i).reason);
                break;
            }
        }
        setPreviewMessage(tr("This combination still does not decode.\n\n%1\n\n%2")
                              .arg(error, hint));
        m_status->clear();
        enableOpen(false);
        return;
    }

    m_result = *spliced;
    m_resultOffset = offset;
    m_haveResult = true;

    m_previewImage = rgb->toImage();
    m_preview->setText(QString());
    showPreviewImage();

    QStringList notes;
    notes << tr("%1 of donor header + %2 of data from the damaged file")
                 .arg(humanSize(m_result.headerSize), humanSize(m_broken.size() - offset));
    notes << (m_result.carriedExif
                  ? tr("the damaged file's own Exif survived and is kept")
                  : tr("the donor's Exif, XMP and comments are dropped, so the rescued file "
                       "carries no other photograph's dates"));
    if (!m_result.keptOwn.isEmpty())
        notes << tr("kept this file's own %1").arg(m_result.keptOwn.join(QStringLiteral(", ")));
    if (m_result.renumberedRestarts > 0)
        notes << tr("%n restart marker(s) renumbered", nullptr, m_result.renumberedRestarts);
    if (m_result.droppedFooter > 0)
        notes << tr("ransomware footer (%1) dropped").arg(humanSize(m_result.droppedFooter));
    m_status->setText(notes.join(QStringLiteral(" · ")) + QStringLiteral("."));
    enableOpen(true);
}
