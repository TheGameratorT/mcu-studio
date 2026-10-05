// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// A folder of rescued files, sorted by what is wrong with each.
//
// A recovery seldom involves one photograph. A card that went bad, a folder
// that ransomware went through: hundreds of files, most fine, some needing a
// donor header, some truncated, some with a desync to chase. Triage first says
// which is which, so the work goes where it is needed.
#pragma once

#include <QDialog>
#include <QFutureWatcher>

#include "Analysis.h"

class QLabel;
class QTreeWidget;

class BatchDialog : public QDialog
{
    Q_OBJECT

public:
    explicit BatchDialog(const QString &folder, QWidget *parent = nullptr);
    ~BatchDialog() override;

signals:
    void openRequested(const QString &path);

private:
    struct Row {
        QString path;
        analysis::Triage triage;
        qint64 size = 0;
    };
    void exportCsv();

    QStringList m_paths;
    QFutureWatcher<Row> m_watcher;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_status = nullptr;
    QVector<Row> m_rows;
};
