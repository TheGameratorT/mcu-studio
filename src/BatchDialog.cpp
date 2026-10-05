// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "BatchDialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {
// Files this large are read whole for triage; anything bigger is unlikely to
// be a single photograph.
constexpr qint64 kMaxBytes = 256LL * 1024 * 1024;
} // namespace

BatchDialog::BatchDialog(const QString &folder, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Triage: %1").arg(QDir::toNativeSeparators(folder)));
    QVBoxLayout *v = new QVBoxLayout(this);
    m_status = new QLabel(this);
    v->addWidget(m_status);
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderLabels({tr("File"), tr("Verdict"), tr("Size"), tr("Picture"), tr("Details")});
    m_tree->setRootIsDecorated(false);
    m_tree->setSortingEnabled(true);
    m_tree->setAlternatingRowColors(true);
    v->addWidget(m_tree, 1);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        emit openRequested(item->data(0, Qt::UserRole).toString());
    });

    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton *open = buttons->addButton(tr("Open"), QDialogButtonBox::ActionRole);
    QPushButton *csv = buttons->addButton(tr("Export CSV…"), QDialogButtonBox::ActionRole);
    connect(open, &QPushButton::clicked, this, [this] {
        if (QTreeWidgetItem *item = m_tree->currentItem())
            emit openRequested(item->data(0, Qt::UserRole).toString());
    });
    connect(csv, &QPushButton::clicked, this, &BatchDialog::exportCsv);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    v->addWidget(buttons);

    QDirIterator it(folder, {QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.JPG"),
                             QStringLiteral("*.JPEG"), QStringLiteral("*.jpe"), QStringLiteral("*.jfif")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        m_paths << it.next();
    // Ransomware renames files with its own extension; offer those too.
    QDirIterator any(folder, QDir::Files, QDirIterator::Subdirectories);
    while (any.hasNext()) {
        const QString p = any.next();
        const QString lower = p.toLower();
        if (lower.contains(QStringLiteral(".jpg.")) || lower.contains(QStringLiteral(".jpeg.")))
            m_paths << p;
    }
    m_paths.removeDuplicates();

    connect(&m_watcher, &QFutureWatcherBase::resultReadyAt, this, [this](int i) {
        const Row r = m_watcher.resultAt(i);
        m_rows.append(r);
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, QDir(QFileInfo(r.path).absolutePath()).relativeFilePath(r.path).isEmpty()
                             ? QFileInfo(r.path).fileName()
                             : QFileInfo(r.path).fileName());
        item->setToolTip(0, r.path);
        item->setData(0, Qt::UserRole, r.path);
        item->setText(1, analysis::verdictName(r.triage.verdict));
        item->setText(2, QLocale().formattedDataSize(r.size));
        item->setData(2, Qt::UserRole, r.size);
        item->setText(3, r.triage.width ? QStringLiteral("%1 × %2").arg(r.triage.width).arg(r.triage.height)
                                        : QString());
        item->setText(4, r.triage.summary);
        m_status->setText(tr("%1 of %2 file(s) checked.").arg(m_rows.size()).arg(m_paths.size()));
    });
    connect(&m_watcher, &QFutureWatcherBase::finished, this, [this] {
        QMap<QString, int> counts;
        for (const Row &r : std::as_const(m_rows))
            counts[analysis::verdictName(r.triage.verdict)]++;
        QStringList parts;
        for (auto c = counts.constBegin(); c != counts.constEnd(); ++c)
            parts << QStringLiteral("%1: %2").arg(c.key()).arg(c.value());
        m_status->setText(tr("%n file(s). ", nullptr, int(m_rows.size())) + parts.join(QStringLiteral(" · ")));
        m_tree->header()->resizeSections(QHeaderView::ResizeToContents);
        m_tree->sortByColumn(1, Qt::AscendingOrder);
    });

    m_status->setText(tr("Checking %n file(s)…", nullptr, int(m_paths.size())));
    m_watcher.setFuture(QtConcurrent::mapped(m_paths, [](const QString &path) {
        Row r;
        r.path = path;
        QFile f(path);
        r.size = f.size();
        if (r.size <= kMaxBytes && f.open(QIODevice::ReadOnly))
            r.triage = analysis::triage(f.readAll());
        else
            r.triage.summary = QStringLiteral("not read");
        return r;
    }));
    resize(1000, 560);
}

BatchDialog::~BatchDialog()
{
    m_watcher.cancel();
    m_watcher.waitForFinished();
}

void BatchDialog::exportCsv()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Export triage"), QString(),
                                                      tr("CSV files (*.csv)"));
    if (path.isEmpty())
        return;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("Could not export"), f.errorString());
        return;
    }
    const auto quote = [](QString s) { return QLatin1Char('"') + s.replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"'); };
    QByteArray out = "path,verdict,bytes,width,height,mcus,decode_errors,first_error,readable,details\n";
    for (const Row &r : std::as_const(m_rows)) {
        out += QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10\n")
                   .arg(quote(r.path), quote(analysis::verdictName(r.triage.verdict)))
                   .arg(r.size)
                   .arg(r.triage.width)
                   .arg(r.triage.height)
                   .arg(r.triage.mcus)
                   .arg(r.triage.decodeErrors)
                   .arg(r.triage.firstError)
                   .arg(r.triage.readable, 0, 'f', 3)
                   .arg(quote(r.triage.summary))
                   .toUtf8();
    }
    if (f.write(out) != out.size() || !f.commit())
        QMessageBox::warning(this, tr("Could not export"), f.errorString());
}
