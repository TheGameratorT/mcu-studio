// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "EmbeddedImagesDialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "JpegRepair.h"

EmbeddedImagesDialog::EmbeddedImagesDialog(const QByteArray &fileBytes, const QString &sourcePath,
                                           QWidget *parent)
    : QDialog(parent)
    , m_bytes(fileBytes)
    , m_sourcePath(sourcePath)
    , m_found(jpegfile::embeddedJpegs(fileBytes))
{
    setWindowTitle(tr("Pictures inside %1").arg(QFileInfo(sourcePath).fileName()));
    QVBoxLayout *v = new QVBoxLayout(this);
    QLabel *hint = new QLabel(
        tr("Cameras store smaller copies of the photograph alongside it: an Exif thumbnail, often a "
           "larger preview. They are written separately from the main picture data, so they "
           "frequently survive damage to it, and they show the same scene -- the best reference "
           "for color, or for filling blocks with nothing left in them."),
        this);
    hint->setWordWrap(true);
    v->addWidget(hint);

    QHBoxLayout *h = new QHBoxLayout;
    m_list = new QListWidget(this);
    for (const jpegfile::Embedded &e : m_found) {
        m_list->addItem(tr("%1  ·  %2 × %3  ·  %4 bytes at %5")
                            .arg(e.origin)
                            .arg(e.width)
                            .arg(e.height)
                            .arg(e.length)
                            .arg(e.offset));
    }
    h->addWidget(m_list, 1);
    m_preview = new QLabel(this);
    m_preview->setMinimumSize(320, 240);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setFrameShape(QFrame::StyledPanel);
    h->addWidget(m_preview, 1);
    v->addLayout(h, 1);

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton *save = buttons->addButton(tr("Save As…"), QDialogButtonBox::ActionRole);
    QPushButton *color = buttons->addButton(tr("Use as Color Reference"), QDialogButtonBox::ActionRole);
    QPushButton *fill = buttons->addButton(tr("Use as Fill Reference"), QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, [this] {
        const QByteArray bytes = selectedBytes();
        if (bytes.isEmpty())
            return;
        const QFileInfo src(m_sourcePath);
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Save embedded picture"),
            src.dir().filePath(src.completeBaseName() + QStringLiteral("_embedded_%1.jpg").arg(m_list->currentRow() + 1)),
            tr("JPEG images (*.jpg)"));
        if (path.isEmpty())
            return;
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
            QMessageBox::warning(this, tr("Could not save"), f.errorString());
    });
    connect(color, &QPushButton::clicked, this, [this] {
        if (writeTemp()) {
            m_use = Use::ColorReference;
            accept();
        }
    });
    connect(fill, &QPushButton::clicked, this, [this] {
        if (writeTemp()) {
            m_use = Use::FillReference;
            accept();
        }
    });
    v->addWidget(buttons);

    for (QPushButton *b : {save, color, fill})
        b->setEnabled(!m_found.isEmpty());
    connect(m_list, &QListWidget::currentRowChanged, this, &EmbeddedImagesDialog::showSelected);
    if (!m_found.isEmpty())
        m_list->setCurrentRow(0);
    else
        m_preview->setText(tr("This file carries no other complete JPEG."));
    resize(860, 460);
}

QByteArray EmbeddedImagesDialog::selectedBytes() const
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_found.size())
        return QByteArray();
    return m_bytes.mid(m_found[row].offset, m_found[row].length);
}

void EmbeddedImagesDialog::showSelected()
{
    const auto rgb = jr::decodeRgb(selectedBytes());
    if (!rgb) {
        m_preview->setText(tr("Does not decode."));
        return;
    }
    m_preview->setPixmap(QPixmap::fromImage(rgb->toImage())
                             .scaled(m_preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

bool EmbeddedImagesDialog::writeTemp()
{
    const QByteArray bytes = selectedBytes();
    if (bytes.isEmpty())
        return false;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    m_path = QDir(dir).filePath(QStringLiteral("mcu-studio-%1-embedded-%2.jpg")
                                    .arg(QFileInfo(m_sourcePath).completeBaseName())
                                    .arg(m_list->currentRow() + 1));
    QSaveFile f(m_path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) {
        QMessageBox::warning(this, tr("Could not write a temporary copy"), f.errorString());
        return false;
    }
    return true;
}
