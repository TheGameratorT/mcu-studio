// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "ReferenceFillDialog.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QSpinBox>
#include <QVBoxLayout>

#include "PlatformStyle.h"
#include "ReferenceImage.h"

namespace {

// How far the alignment can be nudged. Wide enough to chase a stream that a
// donor header has shifted a long way, and bounded so the spin boxes stay
// usable.
constexpr int kMaxOffset = 10000;

// The two panels are the whole point of the dialog, so they get room.
constexpr int kPanelMinWidth = 300;
constexpr int kPanelMinHeight = 220;

// Both panels show whole pictures rather than the selection alone. Damage runs
// in scan order, so a selection is typically a few block rows spanning the full
// width -- a strip 160 times wider than it is tall, which tells you nothing
// when it is scaled to fit a panel. What the eye can actually settle here is
// whether this is the same photograph and whether the marked region is over the
// right part of it; judging the fill block by block is what the main window's
// zoom and the step list's untick are for.
constexpr int kPreviewLongEdge = 1400;

// Enough of the panel for the region marker to be findable when the selection
// is a single block in a 3888-pixel picture.
constexpr double kMinMarkerPixels = 3.0;

QImage downscaled(const QImage &image, int longEdge)
{
    if (image.isNull() || qMax(image.width(), image.height()) <= longEdge)
        return image;
    return image.width() >= image.height()
        ? image.scaledToWidth(longEdge, Qt::SmoothTransformation)
        : image.scaledToHeight(longEdge, Qt::SmoothTransformation);
}

// The picture scaled into `target`, with `region` -- in the picture's own
// pixels -- drawn over it. A light stroke over a dark one so the marker reads
// on any content, which after this kind of damage may be anything at all.
QPixmap withRegion(const QImage &picture, QSize pictureSize, QRect region, QSize target)
{
    if (picture.isNull() || target.isEmpty() || pictureSize.isEmpty())
        return QPixmap();

    const QImage fitted = picture.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPixmap pixmap = QPixmap::fromImage(fitted);
    if (region.isEmpty())
        return pixmap;

    const double sx = double(fitted.width()) / pictureSize.width();
    const double sy = double(fitted.height()) / pictureSize.height();
    QRectF marker(region.x() * sx, region.y() * sy, qMax(region.width() * sx, kMinMarkerPixels),
                  qMax(region.height() * sy, kMinMarkerPixels));

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(QColor(0, 0, 0, 170), 3));
    painter.drawRect(marker);
    painter.setPen(QPen(QColor(255, 210, 60), 1));
    painter.drawRect(marker);
    return pixmap;
}

} // namespace

ReferenceFillDialog::ReferenceFillDialog(const QByteArray &destJpeg, const jr::Info &dest,
                                         const jr::Samples &destRgb, const QByteArray &mask,
                                         const QString &startDir, QWidget *parent)
    : QDialog(parent)
    , m_destJpeg(destJpeg)
    , m_dest(dest)
    , m_destRgb(destRgb)
    , m_mask(mask)
{
    setWindowTitle(tr("Fill Blocks from a Reference Picture"));
    m_plan = fill::planFor(m_mask, m_dest);
    buildUi(startDir);
}

void ReferenceFillDialog::buildUi(const QString &startDir)
{
    QVBoxLayout *layout = new QVBoxLayout(this);

    QLabel *intro = new QLabel(
        tr("Every other repair here moves coefficients the file already has. Where the picture "
           "data is simply gone (overwritten, truncated, decoding to nothing) there is nothing "
           "left inside to move, and content has to come from another copy of the photograph: a "
           "re-render, an export, a backup, a PNG an earlier recovery produced.\n\n"
           "Those pixels have never been through a DCT, so they cannot be transplanted the way "
           "another JPEG's blocks can; they are compressed on the way in, with this file's own "
           "quantization tables. That costs one compression generation on the blocks being "
           "filled, and nothing at all anywhere else: every block outside the selection keeps "
           "the exact coefficients it has now."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    QFormLayout *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    QHBoxLayout *pathRow = new QHBoxLayout;
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setReadOnly(true);
    m_pathEdit->setPlaceholderText(tr("No picture chosen yet"));
    QPushButton *browse = new QPushButton(tr("Choose…"), this);
    connect(browse, &QPushButton::clicked, this, &ReferenceFillDialog::onBrowse);
    pathRow->addWidget(m_pathEdit, 1);
    pathRow->addWidget(browse);
    form->addRow(tr("Reference picture:"), pathRow);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    form->addRow(QString(), m_status);
    layout->addLayout(form);

    // --- the two panels ----------------------------------------------------
    QHBoxLayout *panels = new QHBoxLayout;
    const auto addPanel = [this, panels](const QString &title, const QString &tip, QLabel **view) {
        QGroupBox *box = new QGroupBox(title, this);
        box->setToolTip(tip);
        QVBoxLayout *boxLayout = new QVBoxLayout(box);
        *view = new QLabel(box);
        (*view)->setAlignment(Qt::AlignCenter);
        (*view)->setMinimumSize(kPanelMinWidth, kPanelMinHeight);
        (*view)->setFrameShape(QFrame::StyledPanel);
        boxLayout->addWidget(*view);
        panels->addWidget(box, 1);
    };
    addPanel(tr("The picture being repaired"),
             tr("The marked region is the block selection about to be filled."),
             &m_currentView);
    addPanel(tr("The reference"),
             tr("The marked region is where the fill reads from. Shift it with ΔRow and ΔCol "
                "until it covers the content those blocks should be showing."),
             &m_referenceView);

    layout->addLayout(panels, 1);

    // --- alignment ---------------------------------------------------------
    QHBoxLayout *offsetRow = new QHBoxLayout;
    QLabel *offsetHint = new QLabel(tr("Read the reference from:"), this);
    offsetHint->setToolTip(tr("Shifts which part of the reference the selected blocks are "
                              "filled from, in whole MCUs. Needed whenever the stream has "
                              "moved: a transplanted header, or an insert or delete not yet "
                              "resolved, leaves these blocks showing content from somewhere "
                              "else in the picture."));
    m_rowSpin = new QSpinBox(this);
    m_rowSpin->setRange(-kMaxOffset, kMaxOffset);
    m_rowSpin->setAccelerated(true);
    m_rowSpin->setToolTip(offsetHint->toolTip());
    m_colSpin = new QSpinBox(this);
    m_colSpin->setRange(-kMaxOffset, kMaxOffset);
    m_colSpin->setAccelerated(true);
    m_colSpin->setToolTip(offsetHint->toolTip());
    connect(m_rowSpin, &QSpinBox::valueChanged, this, &ReferenceFillDialog::onOffsetChanged);
    connect(m_colSpin, &QSpinBox::valueChanged, this, &ReferenceFillDialog::onOffsetChanged);

    m_resetOffsetButton = new QPushButton(tr("Line up"), this);
    m_resetOffsetButton->setToolTip(tr("Back to no shift: each block filled from where it sits."));
    connect(m_resetOffsetButton, &QPushButton::clicked, this, [this] {
        m_rowSpin->setValue(0);
        m_colSpin->setValue(0);
    });

    offsetRow->addWidget(offsetHint);
    offsetRow->addWidget(new QLabel(tr("ΔRow:"), this));
    offsetRow->addWidget(m_rowSpin);
    offsetRow->addWidget(new QLabel(tr("ΔCol:"), this));
    offsetRow->addWidget(m_colSpin);
    offsetRow->addWidget(m_resetOffsetButton);
    offsetRow->addStretch(1);
    layout->addLayout(offsetRow);

    m_costLabel = new QLabel(this);
    m_costLabel->setWordWrap(true);
    layout->addWidget(m_costLabel);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_fillButton = m_buttons->addButton(tr("Fill blocks"), QDialogButtonBox::AcceptRole);
    m_fillButton->setDefault(true);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &ReferenceFillDialog::onFill);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);

    // The left panel is the picture being repaired and does not wait on a
    // reference, so it is scaled once here rather than on every repaint.
    m_currentPreview = downscaled(m_destRgb.toImage(), kPreviewLongEdge);
    // Kept for the file dialog, which should open where the picture being
    // repaired came from rather than wherever Qt last was.
    m_pathEdit->setProperty("startDir", startDir);

    refreshStatus();
    showPanels();
    updateButtons();
    resize(sizeHint().expandedTo(QSize(820, 640)));
}

// ---------------------------------------------------------------------------
// The reference picture
// ---------------------------------------------------------------------------

void ReferenceFillDialog::onBrowse()
{
    const QString startDir =
        m_path.isEmpty() ? m_pathEdit->property("startDir").toString() : m_path;
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose a reference picture"), startDir,
        tr("Images (*.png *.jpg *.jpeg *.tif *.tiff *.bmp *.webp *.PNG *.JPG *.JPEG);;"
           "All files (*)"));
    if (!path.isEmpty())
        setReference(path);
}

bool ReferenceFillDialog::setReference(const QString &path)
{
    QString error;
    const auto loaded = refimage::load(path, &error);
    if (!loaded || !loaded->isValid()) {
        m_status->setStyleSheet(warningTextStyle());
        m_status->setText(error.isEmpty() ? tr("That picture could not be read.") : error);
        return false;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto aligned = fill::align(loaded->preview, m_dest, &error);
    QApplication::restoreOverrideCursor();
    if (!aligned) {
        m_status->setStyleSheet(warningTextStyle());
        m_status->setText(error);
        return false;
    }

    m_path = path;
    m_aligned = *aligned;
    m_pathEdit->setText(path);
    refreshStatus();
    refreshPanels();
    return true;
}

void ReferenceFillDialog::refreshStatus()
{
    if (!m_plan.isValid()) {
        m_status->setStyleSheet(warningTextStyle());
        m_status->setText(tr("No blocks are selected. Close this, select the damaged blocks, "
                             "and open it again."));
        return;
    }
    if (!m_aligned) {
        m_status->setStyleSheet(QString());
        m_status->setText(tr("Any picture format this system can read will do, including PNG. "
                             "A copy at the original size lends the most detail."));
        return;
    }

    QStringList lines;
    const QSize source = m_aligned->sourceSize;
    if (!m_aligned->scaled) {
        lines << tr("%1 × %2 px, the same size as the picture being repaired.")
                     .arg(source.width())
                     .arg(source.height());
    } else {
        lines << tr("%1 × %2 px, scaled to this file's %3 × %4. A copy at the original size "
                    "would lend sharper blocks.")
                     .arg(source.width())
                     .arg(source.height())
                     .arg(m_dest.width)
                     .arg(m_dest.height);
    }
    if (m_aligned->aspectChanged) {
        lines << tr("Its proportions differ from this file's, so scaling stretched it. That "
                    "usually means it is a different crop, and a different crop will not line "
                    "up block for block.");
    }

    m_status->setStyleSheet(m_aligned->aspectChanged ? warningTextStyle() : QString());
    m_status->setText(lines.join(QStringLiteral(" ")));
}

// ---------------------------------------------------------------------------
// The panels
// ---------------------------------------------------------------------------

void ReferenceFillDialog::onOffsetChanged()
{
    m_offset = QPoint(m_colSpin->value(), m_rowSpin->value());
    refreshPanels();
}

void ReferenceFillDialog::refreshPanels()
{
    m_referencePreview =
        m_aligned ? downscaled(m_aligned->image, kPreviewLongEdge) : QImage();
    showPanels();
    updateButtons();
}

QRect ReferenceFillDialog::selectionPixels() const
{
    if (!m_plan.isValid())
        return QRect();
    return QRect(m_plan.mcuRect.x() * m_dest.mcuWidth, m_plan.mcuRect.y() * m_dest.mcuHeight,
                 m_plan.mcuRect.width() * m_dest.mcuWidth,
                 m_plan.mcuRect.height() * m_dest.mcuHeight);
}

void ReferenceFillDialog::showPanels()
{
    const QRect selection = selectionPixels();

    const auto paint = [](QLabel *view, const QImage &picture, QSize pictureSize, QRect region,
                          const QString &placeholder) {
        if (picture.isNull()) {
            view->setPixmap(QPixmap());
            view->setText(placeholder);
            return;
        }
        view->setText(QString());
        view->setPixmap(withRegion(picture, pictureSize, region, view->size()));
    };

    paint(m_currentView, m_currentPreview, QSize(m_dest.width, m_dest.height), selection,
          tr("Nothing to show."));
    // The reference is drawn at its padded size, which is what the offset is
    // measured against, so a region shifted off the picture is visibly off it.
    paint(m_referenceView, m_referencePreview,
          m_aligned ? m_aligned->image.size() : QSize(),
          selection.translated(m_offset.x() * m_dest.mcuWidth, m_offset.y() * m_dest.mcuHeight),
          tr("Choose a reference picture."));

    if (!m_plan.isValid()) {
        m_costLabel->clear();
        return;
    }

    const QLocale locale;
    QString text = tr("Filling %1 of the %2 blocks in this picture. The %3 × %4 blocks the "
                      "selection spans are compressed once, with this file's own quantization "
                      "tables; every other block keeps its exact coefficients.")
                       .arg(locale.toString(m_plan.mcuCount), locale.toString(m_dest.mcuCount()))
                       .arg(m_plan.mcuRect.width())
                       .arg(m_plan.mcuRect.height());
    if (m_offset != QPoint(0, 0)) {
        text += QLatin1Char(' ')
            + tr("Read from %1 block row(s) and %2 column(s) away.")
                  .arg(m_offset.y())
                  .arg(m_offset.x());
    }
    m_costLabel->setText(text);
}

void ReferenceFillDialog::updateButtons()
{
    m_fillButton->setEnabled(m_plan.isValid() && m_aligned.has_value());
    m_resetOffsetButton->setEnabled(m_offset != QPoint(0, 0));
    const bool haveReference = m_aligned.has_value();
    m_rowSpin->setEnabled(haveReference);
    m_colSpin->setEnabled(haveReference);
}

void ReferenceFillDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    showPanels();
}

// ---------------------------------------------------------------------------
// Committing
// ---------------------------------------------------------------------------

void ReferenceFillDialog::onFill()
{
    if (!m_plan.isValid() || !m_aligned)
        return;

    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto op = fill::build(m_destJpeg, m_dest, *m_aligned, m_mask, m_plan, m_offset, &error);
    QApplication::restoreOverrideCursor();

    if (!op) {
        m_status->setStyleSheet(warningTextStyle());
        m_status->setText(error.isEmpty() ? tr("Those blocks could not be filled.") : error);
        return;
    }
    m_op = *op;
    accept();
}
