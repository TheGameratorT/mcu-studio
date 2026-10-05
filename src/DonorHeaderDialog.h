// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// Picking a donor header for a file that will not open, and choosing where the
// damaged file's own data resumes.
//
// Both choices are guesses, so the dialog is built around a preview rather
// than around a wizard: splice, decode, look. A wrong donor is obvious at a
// glance -- wrong tables give confetti, wrong dimensions give a diagonal
// smear -- and a wrong splice point shows as an image that starts too early or
// too late. The controls are there to be swept through.
#pragma once

#include <QByteArray>
#include <QDialog>
#include <QImage>
#include <QString>
#include <QVector>

#include "DonorHeader.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;

class DonorHeaderDialog : public QDialog
{
    Q_OBJECT

public:
    DonorHeaderDialog(const QString &brokenPath, const QByteArray &brokenBytes,
                      QWidget *parent = nullptr);

    // Valid once the dialog has been accepted.
    const QByteArray &splicedBytes() const { return m_result.bytes; }
    QString donorPath() const { return m_donorPath; }
    qsizetype spliceOffset() const { return m_resultOffset; }
    bool carriedExif() const { return m_result.carriedExif; }
    qsizetype headerSize() const { return m_result.headerSize; }
    donor::SpliceOptions spliceOptions() const;

protected:
    // Judging a splice means looking closely, so the preview follows the
    // window when it is dragged bigger.
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onBrowseDonor();
    void onCandidateChosen(int index);
    void onOffsetEdited();
    void onRankDonors();
    void onDetectWidth();
    void rebuild();

private:
    void buildUi(const QString &brokenPath);
    // Reads `path`, scans it, and takes it as the donor if it can serve as one.
    // Reports why not in the donor status line otherwise.
    bool setDonor(const QString &path);
    // A JPEG from the same folder is the likeliest sibling from the same camera
    // roll, so one is offered up front rather than made the user's first chore.
    void suggestDonorFromFolder(const QString &brokenPath);
    void refreshCandidates();
    void schedulePreview();
    void setPreviewMessage(const QString &message);
    void showPreviewImage(); // scales m_previewImage into the label as it stands

    QByteArray m_broken;
    donor::Layout m_brokenLayout;

    QString m_donorPath;
    QByteArray m_donorBytes;
    donor::Layout m_donorLayout;
    QString m_lastDonorError; // why the last candidate was turned down

    QVector<donor::SplicePoint> m_candidates;
    QImage m_previewImage;
    donor::Splice m_result;
    qsizetype m_resultOffset = 0;
    bool m_haveResult = false;

    QLabel *m_brokenLabel = nullptr;
    QLineEdit *m_donorEdit = nullptr;
    QLabel *m_donorStatus = nullptr;
    QComboBox *m_candidateCombo = nullptr;
    QSpinBox *m_offsetSpin = nullptr;
    QCheckBox *m_keepOwnCheck = nullptr;
    QCheckBox *m_renumberCheck = nullptr;
    QSpinBox *m_widthSpin = nullptr;
    QSpinBox *m_heightSpin = nullptr;
    QSpinBox *m_restartSpin = nullptr;
    QLabel *m_djvuLabel = nullptr;
    QLabel *m_preview = nullptr;
    QLabel *m_status = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
    QTimer *m_debounce = nullptr;

    bool m_updatingControls = false;
};
