// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// Asking a model to make up what the selected MCUs lost, and looking at the
// answer before any of it goes into the file.
//
// The answer is invented, and how good an invention it is varies from one run
// to the next and from one region to the next. So nothing is committed from
// here until it has been looked at: each region is shown as it is now beside
// what would replace it, the model can be asked again, and only Fill MCUs
// hands a step back to the main window.
#pragma once

#include <QByteArray>
#include <QDialog>
#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

#include <memory>
#include <optional>
#include <vector>

#include "AiFill.h"
#include "JpegRepair.h"
#include "ReferenceFill.h"

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;

class AiFillDialog : public QDialog
{
    Q_OBJECT

public:
    // `destJpeg` is the image being repaired as it currently stands, `destRgb`
    // its decoded pixels, and `mask` the selected MCUs, one byte each,
    // row-major.
    AiFillDialog(const QByteArray &destJpeg, const jr::Info &dest, const jr::Samples &destRgb,
                 const QByteArray &mask, QWidget *parent = nullptr);

    // Valid once the dialog has been accepted.
    const jr::Op &op() const { return m_op; }
    int mcuCount() const { return m_plan.mcuCount; }
    int regionCount() const { return int(m_regions.size()); }
    QString providerName() const { return m_usedProvider; }

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onProviderChanged();
    void onSetUp();
    void onGenerate();
    void onFill();

private:
    void buildUi();
    // Rebuilds the provider list from the stored settings, keeping the choice.
    void reloadProviders();
    aifill::Provider *currentProvider() const;
    void refreshNotice();
    void showPanels();
    void updateButtons();
    void setStatus(const QString &text, bool warning);
    // The part of the picture shown for region `index`: the region with
    // enough around it to judge whether the fill belongs there.
    QRect viewRect(int index) const;

    QByteArray m_destJpeg;
    jr::Info m_dest;
    jr::Samples m_destRgb;
    QByteArray m_mask;
    fill::Plan m_plan;
    QVector<QRect> m_regions; // in MCUs
    int m_region = 0;

    aifill::Settings m_settings;
    std::vector<std::unique_ptr<aifill::Provider>> m_providers;

    QImage m_current;
    std::optional<aifill::Result> m_result;
    QString m_usedProvider;
    jr::Op m_op;

    QComboBox *m_providerCombo = nullptr;
    QLabel *m_providerNote = nullptr;
    QLineEdit *m_promptEdit = nullptr;
    QComboBox *m_uprightCombo = nullptr;
    QLabel *m_currentView = nullptr;
    QLabel *m_resultView = nullptr;
    QLabel *m_regionLabel = nullptr;
    QPushButton *m_previousButton = nullptr;
    QPushButton *m_nextButton = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_generateButton = nullptr;
    QPushButton *m_fillButton = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};
