// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// Pointing at a patch of another copy of the picture and measuring its color.
//
// Like the donor header dialog, this is built around looking rather than around
// a wizard: the patch is dragged over the picture and the mean under it is
// re-read as it moves, shown both as numbers and as the color those numbers
// stand for. What matters is that the patch ends up on the same *content* as
// the target block, and only an eye can judge that -- the geometry cannot,
// because a transplanted header leaves the stream shifted by an unknown amount.
#pragma once

#include <QDialog>
#include <QFrame>
#include <QImage>
#include <QPixmap>
#include <QRect>
#include <QSize>
#include <QString>

#include <functional>

#include "ColorMath.h"
#include "ReferenceImage.h"

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

// The picture with a movable measuring patch drawn over it.
//
// Deliberately not a signal emitter: the dialog is its only user and owns it
// outright, so a callback says the same thing without a second moc pass.
class ReferencePatchView : public QFrame
{
public:
    explicit ReferencePatchView(QWidget *parent = nullptr);

    void setImage(const QImage &image);
    QRect patch() const { return m_patch; }
    void setPatch(QRect patch); // in image pixels
    // Shown centerd while there is no picture to draw, so the panel says what
    // it is waiting for rather than sitting there empty.
    void setPlaceholder(const QString &text);

    // Called when a drag moves the patch. Not called by setPatch(), so a caller
    // driving the patch cannot be re-entered by its own change.
    std::function<void(QRect)> patchDragged;

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    // Where the image is drawn inside the widget, letterboxed to fit.
    QRectF drawnRect() const;
    void rescale();
    void dragTo(const QPointF &widgetPos);

    QImage m_image;
    QPixmap m_scaled; // m_image at the current widget size, cached per resize
    QRect m_patch;
    QString m_placeholder;
};

class ReferenceColorDialog : public QDialog
{
    Q_OBJECT

public:
    // `subjectSize` is the size of the picture being repaired and `targetRect`
    // the pixel rectangle of its picked target block, or a null rectangle when
    // no target has been picked -- together they place the patch where the
    // target block would fall in this copy.
    ReferenceColorDialog(const QString &path, QSize subjectSize, QRect targetRect,
                          const QString &targetName, QWidget *parent = nullptr);

    // Valid once the dialog has been accepted.
    QString referencePath() const { return m_path; }
    QRect patch() const { return m_view->patch(); }
    const colormath::BlockStats &stats() const { return m_stats; }
    // Grayscale, so its Cb and Cr are a flat 128 and not a color to match.
    bool monochrome() const { return m_loaded.monochrome; }

private slots:
    void onBrowse();
    void onPatchSizeChanged(int size);
    void onAlignToTarget();

private:
    void buildUi(const QString &targetName);
    // Reads `path` and, if it decodes, makes it the reference. Reports why not
    // in the status line otherwise, leaving any loaded picture in place.
    bool setReference(const QString &path);
    void setPatch(QRect patch);
    // Re-reads the mean under the patch and rewrites the readout.
    void remeasure();

    QSize m_subjectSize;
    QRect m_targetRect;
    // What the patch controls do, held rather than set once because both are
    // replaced by the reason they cannot be used yet.
    QString m_sizeHint;
    QString m_alignHint;

    QString m_path;
    refimage::Loaded m_loaded;
    colormath::BlockStats m_stats;

    QLineEdit *m_pathEdit = nullptr;
    QLabel *m_status = nullptr;
    ReferencePatchView *m_view = nullptr;
    QSpinBox *m_sizeSpin = nullptr;
    QPushButton *m_alignButton = nullptr;
    QLabel *m_meanLabel = nullptr;
    QLabel *m_swatch = nullptr;
    QLabel *m_warningLabel = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
    QPushButton *m_useButton = nullptr;

    bool m_updatingControls = false;
};
