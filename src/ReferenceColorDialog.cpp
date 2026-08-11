// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "ReferenceColorDialog.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "PlatformStyle.h"

namespace {

constexpr int kViewWidth = 520;
constexpr int kViewHeight = 340;

// A patch outline this small on screen is invisible, and a patch two reference
// pixels across is exactly what a thumbnail gives -- so the outline is drawn no
// smaller than this, around the same center, even when the patch itself is.
constexpr qreal kMinOutlinePx = 11.0;

// Matches the main window's threshold for calling a match unsafe.
constexpr double kClippedSampleWarning = 0.01;

// Below this the mean is one or two pixels' worth of noise rather than a color.
constexpr qint64 kThinPatchSamples = 16;

} // namespace

// ---------------------------------------------------------------------------
// The picture with its measuring patch
// ---------------------------------------------------------------------------

ReferencePatchView::ReferencePatchView(QWidget *parent)
    : QFrame(parent)
{
    setFrameShape(QFrame::StyledPanel);
    setMinimumSize(kViewWidth, kViewHeight);
    setCursor(Qt::CrossCursor);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Base);
}

QSize ReferencePatchView::sizeHint() const
{
    return QSize(kViewWidth, kViewHeight);
}

void ReferencePatchView::setImage(const QImage &image)
{
    m_image = image;
    m_patch = QRect();
    rescale();
    update();
}

void ReferencePatchView::setPatch(QRect patch)
{
    m_patch = patch.intersected(m_image.rect());
    update();
}

void ReferencePatchView::setPlaceholder(const QString &text)
{
    m_placeholder = text;
    update();
}

void ReferencePatchView::rescale()
{
    if (m_image.isNull()) {
        m_scaled = QPixmap();
        return;
    }
    const QSize room = contentsRect().size();
    // Nearest-neighbor when magnifying: a thumbnail blown up to fill the panel
    // should show the pixels it really has, since those are what gets measured.
    const bool magnifying = room.width() > m_image.width() || room.height() > m_image.height();
    m_scaled = QPixmap::fromImage(m_image).scaled(
        room, Qt::KeepAspectRatio,
        magnifying ? Qt::FastTransformation : Qt::SmoothTransformation);
}

QRectF ReferencePatchView::drawnRect() const
{
    if (m_scaled.isNull())
        return QRectF();
    const QRectF room = contentsRect();
    const QSizeF s = m_scaled.size();
    return QRectF(QPointF(room.x() + (room.width() - s.width()) / 2.0,
                          room.y() + (room.height() - s.height()) / 2.0),
                  s);
}

void ReferencePatchView::resizeEvent(QResizeEvent *event)
{
    QFrame::resizeEvent(event);
    rescale();
}

void ReferencePatchView::paintEvent(QPaintEvent *event)
{
    QFrame::paintEvent(event);
    QPainter painter(this);

    const QRectF drawn = drawnRect();
    if (drawn.isEmpty()) {
        if (!m_placeholder.isEmpty()) {
            painter.setPen(palette().color(QPalette::Disabled, QPalette::WindowText));
            painter.drawText(contentsRect(), Qt::AlignCenter | Qt::TextWordWrap, m_placeholder);
        }
        return;
    }
    painter.drawPixmap(drawn.topLeft(), m_scaled);

    if (m_patch.isEmpty())
        return;

    const qreal sx = drawn.width() / m_image.width();
    const qreal sy = drawn.height() / m_image.height();
    QRectF outline(drawn.x() + m_patch.x() * sx, drawn.y() + m_patch.y() * sy,
                   m_patch.width() * sx, m_patch.height() * sy);
    if (outline.width() < kMinOutlinePx || outline.height() < kMinOutlinePx) {
        const QPointF center = outline.center();
        outline.setSize(QSizeF(std::max(outline.width(), kMinOutlinePx),
                               std::max(outline.height(), kMinOutlinePx)));
        outline.moveCenter(center);
    }

    // Black under white, so the box reads on any content it is dropped onto.
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Qt::black, 3.0));
    painter.drawRect(outline);
    painter.setPen(QPen(Qt::white, 1.0));
    painter.drawRect(outline);
}

void ReferencePatchView::dragTo(const QPointF &widgetPos)
{
    const QRectF drawn = drawnRect();
    if (drawn.isEmpty() || m_patch.isEmpty())
        return;

    const QPoint center(
        int(std::floor((widgetPos.x() - drawn.x()) / drawn.width() * m_image.width())),
        int(std::floor((widgetPos.y() - drawn.y()) / drawn.height() * m_image.height())));
    const QRect moved = refimage::centerdIn(m_patch, center, m_image.size());
    if (moved == m_patch || moved.isEmpty())
        return;

    m_patch = moved;
    update();
    if (patchDragged)
        patchDragged(m_patch);
}

void ReferencePatchView::mousePressEvent(QMouseEvent *event)
{
    dragTo(event->position());
}

void ReferencePatchView::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton)
        dragTo(event->position());
}

// ---------------------------------------------------------------------------
// The dialog
// ---------------------------------------------------------------------------

ReferenceColorDialog::ReferenceColorDialog(const QString &path, QSize subjectSize,
                                             QRect targetRect, const QString &targetName,
                                             QWidget *parent)
    : QDialog(parent)
    , m_subjectSize(subjectSize)
    , m_targetRect(targetRect)
{
    setWindowTitle(tr("Reference Color from Another Image"));
    buildUi(targetName);
    setReference(path); // reports its own failure in the status line
}

void ReferenceColorDialog::buildUi(const QString &targetName)
{
    QVBoxLayout *layout = new QVBoxLayout(this);

    QLabel *intro = new QLabel(
        tr("Matching color needs one patch of the picture whose color is still right. A "
           "transplanted header does not leave one: the borrowed tables never described this "
           "data, so the very first block can already be wrong and there is nothing inside the "
           "file to measure against.\n\n"
           "Another copy of the same photograph has it (a surviving thumbnail, a phone gallery "
           "cache, a backup, a copy someone was sent). Only the mean color under the box is "
           "read, and color is what survives being shrunk, so a small or soft copy does the "
           "job. Nothing from this file goes into the repair."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    QFormLayout *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    QHBoxLayout *pathRow = new QHBoxLayout;
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setReadOnly(true);
    m_pathEdit->setPlaceholderText(tr("No image chosen yet"));
    QPushButton *browse = new QPushButton(tr("Change…"), this);
    connect(browse, &QPushButton::clicked, this, &ReferenceColorDialog::onBrowse);
    pathRow->addWidget(m_pathEdit, 1);
    pathRow->addWidget(browse);
    form->addRow(tr("Reference image:"), pathRow);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    form->addRow(QString(), m_status);
    layout->addLayout(form);

    m_view = new ReferencePatchView(this);
    m_view->setPlaceholder(tr("Choose an image to measure."));
    layout->addWidget(m_view, 1);

    QHBoxLayout *controls = new QHBoxLayout;
    m_sizeSpin = new QSpinBox(this);
    m_sizeSpin->setSuffix(tr(" px"));
    m_sizeSpin->setAccelerated(true);
    m_sizeSpin->setRange(1, 1);
    m_sizeHint = tr("How wide a patch is averaged, in this image's own pixels. Wider steadies "
                    "the mean; narrower keeps it on one piece of content.");
    connect(m_sizeSpin, &QSpinBox::valueChanged, this,
            &ReferenceColorDialog::onPatchSizeChanged);
    controls->addWidget(new QLabel(tr("Patch:"), this));
    controls->addWidget(m_sizeSpin);
    controls->addSpacing(12);

    m_alignButton = new QPushButton(tr("Put it where the target block is"), this);
    connect(m_alignButton, &QPushButton::clicked, this, &ReferenceColorDialog::onAlignToTarget);
    // Kept on show even when it cannot be used, with the reason in its tooltip:
    // "there is no target block to line up with" is worth knowing.
    m_alignHint = tr("Puts the patch where %1 falls in this copy, scaled for its size. Only a "
                     "starting point: a transplanted header leaves the stream shifted, so check "
                     "by eye that the box is on the same content the target block should be "
                     "showing.")
                      .arg(targetName.isEmpty() ? tr("the target block") : targetName);
    controls->addWidget(m_alignButton);
    controls->addStretch(1);
    layout->addLayout(controls);

    QHBoxLayout *readout = new QHBoxLayout;
    m_swatch = new QLabel(this);
    m_swatch->setFixedSize(30, 20);
    m_swatch->setFrameShape(QFrame::StyledPanel);
    m_swatch->setToolTip(tr("The color the patch averages to."));
    m_meanLabel = new QLabel(this);
    m_meanLabel->setWordWrap(true);
    readout->addWidget(m_swatch);
    readout->addWidget(m_meanLabel, 1);
    layout->addLayout(readout);

    m_warningLabel = new QLabel(this);
    m_warningLabel->setWordWrap(true);
    m_warningLabel->setStyleSheet(warningTextStyle());
    m_warningLabel->hide();
    layout->addWidget(m_warningLabel);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_useButton = m_buttons->addButton(tr("Use as reference"), QDialogButtonBox::AcceptRole);
    m_useButton->setDefault(true);
    m_useButton->setEnabled(false);
    connect(m_buttons, &QDialogButtonBox::accepted, this, [this] {
        if (m_stats.isValid())
            accept();
    });
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);

    m_view->patchDragged = [this](QRect) { remeasure(); };
    // Everything below the picture describes a measurement, so let the one that
    // has not happened yet describe itself too.
    remeasure();
}

// ---------------------------------------------------------------------------
// The reference image
// ---------------------------------------------------------------------------

bool ReferenceColorDialog::setReference(const QString &path)
{
    QString error;
    const auto loaded = refimage::load(path, &error);
    if (!loaded || !loaded->isValid()) {
        m_status->setStyleSheet(warningTextStyle());
        m_status->setText(error.isEmpty() ? tr("That image could not be read.") : error);
        return false;
    }

    m_path = path;
    m_loaded = *loaded;
    const QSize size = m_loaded.size();

    m_pathEdit->setText(path);
    m_status->setStyleSheet(QString());
    m_status->setText(tr("%1 × %2 px").arg(size.width()).arg(size.height()));
    m_view->setImage(m_loaded.preview);

    // A patch scaled from the target block is the size that means something
    // here; without a target, something small enough to sit on one piece of
    // content and big enough to average over.
    QRect patch = refimage::correspondingRect(m_targetRect, m_subjectSize, size);
    if (patch.isEmpty()) {
        const int edge = std::clamp(std::min(size.width(), size.height()) / 16, 2, 32);
        patch = refimage::centerdIn(QRect(0, 0, edge, edge),
                                    QPoint(size.width() / 2, size.height() / 2), size);
    }

    m_updatingControls = true;
    m_sizeSpin->setRange(1, std::min(size.width(), size.height()));
    m_sizeSpin->setValue(std::max(patch.width(), patch.height()));
    m_updatingControls = false;

    setPatch(patch);
    return true;
}

void ReferenceColorDialog::onBrowse()
{
    const QString start = m_path.isEmpty() ? QString() : QFileInfo(m_path).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose a reference image"), start,
        tr("Images (*.jpg *.jpeg *.JPG *.JPEG *.png *.bmp *.tif *.tiff *.webp);;All files (*)"));
    if (!path.isEmpty())
        setReference(path);
}

void ReferenceColorDialog::onPatchSizeChanged(int size)
{
    if (m_updatingControls || !m_loaded.isValid())
        return;
    const QRect current = m_view->patch();
    setPatch(refimage::centerdIn(QRect(0, 0, size, size),
                                 current.isEmpty() ? QPoint(m_loaded.size().width() / 2,
                                                            m_loaded.size().height() / 2)
                                                   : current.center(),
                                 m_loaded.size()));
}

void ReferenceColorDialog::onAlignToTarget()
{
    if (!m_loaded.isValid() || m_targetRect.isEmpty())
        return;
    const QRect corresponding =
        refimage::correspondingRect(m_targetRect, m_subjectSize, m_loaded.size());
    setPatch(refimage::centerdIn(QRect(0, 0, m_sizeSpin->value(), m_sizeSpin->value()),
                                 corresponding.center(), m_loaded.size()));
}

void ReferenceColorDialog::setPatch(QRect patch)
{
    m_view->setPatch(patch);
    remeasure();
}

void ReferenceColorDialog::remeasure()
{
    m_stats = colormath::measure(m_loaded.ycbcr, m_view->patch());
    m_useButton->setEnabled(m_stats.isValid());

    const bool haveImage = m_loaded.isValid();
    m_sizeSpin->setEnabled(haveImage);
    m_sizeSpin->setToolTip(haveImage ? m_sizeHint : tr("Choose an image to measure first."));
    m_alignButton->setEnabled(haveImage && !m_targetRect.isEmpty());
    m_alignButton->setToolTip(!haveImage ? tr("Choose an image to measure first.")
                                         : m_targetRect.isEmpty()
                                             ? tr("Pick a target block in the damaged image "
                                                  "first.")
                                             : m_alignHint);

    if (!m_stats.isValid()) {
        m_swatch->hide();
        m_meanLabel->setText(haveImage ? tr("Drag the box onto the picture to measure it.")
                                       : tr("Nothing measured yet."));
        m_warningLabel->hide();
        return;
    }
    m_swatch->show();

    const QRect patch = m_view->patch();
    m_swatch->setStyleSheet(QStringLiteral("background-color: %1;")
                                .arg(colormath::rgbForYCbCr(m_stats.mean).name()));
    m_meanLabel->setText(tr("Mean %1   ·   %2 × %3 px at (%4, %5)")
                             .arg(colormath::formatTriple(m_stats.mean))
                             .arg(patch.width())
                             .arg(patch.height())
                             .arg(patch.x())
                             .arg(patch.y()));

    QStringList warnings;
    if (m_loaded.monochrome) {
        warnings << tr("This image is grayscale, so it has no color of its own to lend: its Cb "
                       "and Cr read as neutral 128 whatever it shows. Use it for brightness and "
                       "leave the Cb and Cr deltas alone, or find a color copy.");
    }
    if (m_stats.sampleCount < kThinPatchSamples) {
        warnings << tr("The patch covers %n pixel(s). Widen it, or the mean is as much noise as "
                       "color.",
                       nullptr, int(m_stats.sampleCount));
    }
    QStringList clipped;
    for (int c = 0; c < 3; ++c) {
        if (m_stats.clipped[c] > kClippedSampleWarning) {
            clipped << QStringLiteral("%1 %2%")
                           .arg(QString::fromLatin1(colormath::channelNames[c]))
                           .arg(m_stats.clipped[c] * 100.0, 0, 'f', 0);
        }
    }
    if (!clipped.isEmpty()) {
        warnings << tr("Samples clipped here (%1): this patch was already at the end of the "
                       "range when it was saved, so its mean understates the real color and "
                       "the match will fall short. Pick somewhere less extreme.")
                        .arg(clipped.join(QStringLiteral(", ")));
    }

    m_warningLabel->setText(warnings.join(QStringLiteral("\n\n")));
    m_warningLabel->setVisible(!warnings.isEmpty());
}
