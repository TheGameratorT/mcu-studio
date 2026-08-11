// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// Choosing a picture to fill damaged blocks from, and lining it up with them.
//
// Like the donor header dialog, this is built around looking. Whether the
// reference is the same photograph, and whether it is sitting where the
// selection thinks it is, are questions only an eye can settle: a header
// transplant or an unresolved insert leaves the stream shifted by some number
// of MCUs, so the blocks under the selection may be showing content from
// somewhere else in the picture entirely. So the two are shown side by side --
// what those blocks hold now, and what the reference would put there -- and the
// alignment is nudged until they are the same thing.
#pragma once

#include <QByteArray>
#include <QDialog>
#include <QImage>
#include <QPoint>
#include <QString>

#include <optional>

#include "JpegRepair.h"
#include "ReferenceFill.h"

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

class ReferenceFillDialog : public QDialog
{
    Q_OBJECT

public:
    // `destJpeg` is the image being repaired as it currently stands, `destRgb`
    // its decoded pixels (for the side-by-side), and `mask` the selected MCUs,
    // one byte each, row-major.
    ReferenceFillDialog(const QByteArray &destJpeg, const jr::Info &dest,
                        const jr::Samples &destRgb, const QByteArray &mask,
                        const QString &startDir, QWidget *parent = nullptr);

    // Valid once the dialog has been accepted.
    const jr::Op &op() const { return m_op; }
    QString referencePath() const { return m_path; }
    QPoint offset() const { return m_offset; }
    int mcuCount() const { return m_plan.mcuCount; }
    // Whether the reference had to be resampled to the damaged file's size,
    // which the log is worth telling: it is the difference between borrowing
    // detail and borrowing an approximation of it.
    bool wasScaled() const { return m_aligned && m_aligned->scaled; }

protected:
    // Judging an alignment means looking closely, so the two panels follow the
    // window when it is dragged bigger.
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onBrowse();
    void onOffsetChanged();
    void onFill();

private:
    void buildUi(const QString &startDir);
    // Reads `path` and lines it up. Reports why not in the status line
    // otherwise, leaving any loaded reference in place.
    bool setReference(const QString &path);
    void refreshPanels();
    void refreshStatus();
    void updateButtons();
    // The block selection's bounding box in the picture's own pixels, which is
    // what both panels mark.
    QRect selectionPixels() const;
    // Redraws both panels at the current widget size. Cheap enough for a
    // resize, since the pictures are downscaled once when they are loaded.
    void showPanels();

    QByteArray m_destJpeg;
    jr::Info m_dest;
    jr::Samples m_destRgb;
    QByteArray m_mask;
    fill::Plan m_plan;

    QString m_path;
    std::optional<fill::Aligned> m_aligned;
    QPoint m_offset;

    jr::Op m_op;

    // Both pictures whole, downscaled once for drawing. The panels mark the
    // selection on them rather than showing it alone -- see the note on
    // kPreviewLongEdge.
    QImage m_currentPreview;
    QImage m_referencePreview;

    QLineEdit *m_pathEdit = nullptr;
    QLabel *m_status = nullptr;
    QSpinBox *m_rowSpin = nullptr;
    QSpinBox *m_colSpin = nullptr;
    QPushButton *m_resetOffsetButton = nullptr;
    QLabel *m_currentView = nullptr;
    QLabel *m_referenceView = nullptr;
    QLabel *m_costLabel = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
    QPushButton *m_fillButton = nullptr;
};
