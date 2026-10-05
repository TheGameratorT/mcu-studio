// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// The other pictures a file carries: the Exif thumbnail, an MPF preview, a
// camera's preview in its maker notes. They sit ahead of or apart from the
// main scan, so they often survive damage that took the main picture, and
// they are the same photograph -- the best reference for color or for
// filling blocks there is.
#pragma once

#include <QByteArray>
#include <QDialog>
#include <QString>
#include <QVector>

#include "JpegStructure.h"

class QLabel;
class QListWidget;

class EmbeddedImagesDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Use { None, ColorReference, FillReference };

    EmbeddedImagesDialog(const QByteArray &fileBytes, const QString &sourcePath, QWidget *parent = nullptr);

    bool isEmpty() const { return m_found.isEmpty(); }
    // After exec(): what the user picked it for, and where it was written.
    Use chosenUse() const { return m_use; }
    QString chosenPath() const { return m_path; }

private:
    void showSelected();
    QByteArray selectedBytes() const;
    bool writeTemp();

    QByteArray m_bytes;
    QString m_sourcePath;
    QVector<jpegfile::Embedded> m_found;
    QListWidget *m_list = nullptr;
    QLabel *m_preview = nullptr;
    Use m_use = Use::None;
    QString m_path;
};
