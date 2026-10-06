// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "AiFillDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QProgressDialog>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QStandardItemModel>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <atomic>

#include "Exif.h"
#include "PlatformStyle.h"

namespace {

constexpr int kPanelMinWidth = 340;
constexpr int kPanelMinHeight = 260;

// How much picture is shown around a region, at least. Enough that a face or
// a horizon the region cuts through can be recognized as one.
constexpr int kViewPadding = 96;
constexpr int kViewMinSide = 320;

QPixmap cropFor(const QImage &picture, const QRect &view, QSize target, const QRect &outline)
{
    if (picture.isNull() || view.isEmpty() || target.isEmpty())
        return QPixmap();
    const QImage fitted =
        picture.copy(view).scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPixmap pixmap = QPixmap::fromImage(fitted);
    if (outline.isEmpty())
        return pixmap;

    const double sx = double(fitted.width()) / view.width();
    const double sy = double(fitted.height()) / view.height();
    const QRectF marker((outline.x() - view.x()) * sx, (outline.y() - view.y()) * sy,
                        outline.width() * sx, outline.height() * sy);
    QPainter painter(&pixmap);
    painter.setPen(QPen(QColor(0, 0, 0, 170), 3));
    painter.drawRect(marker);
    painter.setPen(QPen(QColor(255, 210, 60), 1));
    painter.drawRect(marker);
    return pixmap;
}

// Fetches the local model into place, checking it is the file expected.
// Returns an empty string on success, or why not.
QString downloadModel(QWidget *parent, const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return AiFillDialog::tr("Could not write to %1: %2").arg(path, file.errorString());

    QNetworkAccessManager manager;
    QNetworkReply *reply =
        manager.get(QNetworkRequest(QUrl(QString::fromLatin1(aifill::kLamaModelUrl))));
    QCryptographicHash hash(QCryptographicHash::Sha256);

    QProgressDialog progress(AiFillDialog::tr("Downloading the LaMa model…"),
                             AiFillDialog::tr("Cancel"), 0, 100, parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setValue(0);

    bool writeFailed = false;
    QObject::connect(reply, &QNetworkReply::readyRead, &progress, [&] {
        const QByteArray chunk = reply->readAll();
        hash.addData(chunk);
        if (file.write(chunk) != chunk.size())
            writeFailed = true;
    });
    QObject::connect(reply, &QNetworkReply::downloadProgress, &progress,
                     [&](qint64 got, qint64 total) {
                         if (total > 0)
                             progress.setValue(int(got * 99 / total));
                     });
    QObject::connect(reply, &QNetworkReply::finished, &progress, &QProgressDialog::reset);
    QObject::connect(&progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
    progress.exec();

    const QNetworkReply::NetworkError error = reply->error();
    const QString errorText = reply->errorString();
    reply->deleteLater();

    if (error == QNetworkReply::OperationCanceledError)
        return AiFillDialog::tr("The download was cancelled.");
    if (error != QNetworkReply::NoError)
        return AiFillDialog::tr("The download failed: %1").arg(errorText);
    if (writeFailed)
        return AiFillDialog::tr("Could not write to %1: %2").arg(path, file.errorString());
    if (hash.result().toHex() != QByteArray(aifill::kLamaModelSha256)) {
        return AiFillDialog::tr("The downloaded file is not the expected model (its checksum "
                                "differs), so it was not kept.");
    }
    if (!file.commit())
        return AiFillDialog::tr("Could not write to %1: %2").arg(path, file.errorString());
    return QString();
}

// The settings every backend needs, in one small window. Returns true when
// they were changed and saved.
bool editSettings(QWidget *parent, aifill::Settings *settings)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(AiFillDialog::tr("Set Up AI Fill"));
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    const auto note = [&dialog](const QString &text) {
        QLabel *label = new QLabel(text, &dialog);
        label->setWordWrap(true);
        label->setEnabled(false);
        return label;
    };

    // --- the local model ---------------------------------------------------
    QGroupBox *lamaBox = new QGroupBox(AiFillDialog::tr("LaMa (runs on this computer)"), &dialog);
    QFormLayout *lamaForm = new QFormLayout(lamaBox);
    lamaForm->addRow(note(AiFillDialog::tr(
        "A small model that needs no graphics card. It continues the surrounding "
        "texture into the gap (sand, sky, foliage, cloth) and is quick, but it cannot draw a "
        "subject: across a face it leaves a smooth blur.")));
    QHBoxLayout *modelRow = new QHBoxLayout;
    QLineEdit *lamaPath = new QLineEdit(settings->lamaModelPath, &dialog);
    lamaPath->setPlaceholderText(aifill::defaultLamaModelPath());
    QPushButton *lamaBrowse = new QPushButton(AiFillDialog::tr("Choose…"), &dialog);
    QPushButton *lamaDownload = new QPushButton(AiFillDialog::tr("Download (about 200 MB)"), &dialog);
    modelRow->addWidget(lamaPath, 1);
    modelRow->addWidget(lamaBrowse);
    modelRow->addWidget(lamaDownload);
    lamaForm->addRow(AiFillDialog::tr("Model file:"), modelRow);
    QLabel *lamaState = new QLabel(&dialog);
    lamaState->setWordWrap(true);
    lamaForm->addRow(QString(), lamaState);
    const auto refreshLama = [=] {
        const QString path = lamaPath->text().trimmed().isEmpty() ? aifill::defaultLamaModelPath()
                                                                 : lamaPath->text().trimmed();
        QString whyNot;
        if (!aifill::localModelSupported(&whyNot)) {
            lamaState->setStyleSheet(warningTextStyle());
            lamaState->setText(AiFillDialog::tr("Not available. %1").arg(whyNot));
        } else if (QFileInfo(path).isFile()) {
            lamaState->setStyleSheet(QString());
            lamaState->setText(AiFillDialog::tr("Installed."));
        } else {
            lamaState->setStyleSheet(QString());
            lamaState->setText(AiFillDialog::tr("Not installed yet."));
        }
    };
    const bool lamaSupported = aifill::localModelSupported();
    lamaPath->setEnabled(lamaSupported);
    lamaBrowse->setEnabled(lamaSupported);
    lamaDownload->setEnabled(lamaSupported);
    QObject::connect(lamaPath, &QLineEdit::textChanged, &dialog, refreshLama);
    QObject::connect(lamaBrowse, &QPushButton::clicked, &dialog, [=, &dialog] {
        const QString path = QFileDialog::getOpenFileName(
            &dialog, AiFillDialog::tr("Choose the LaMa model"), QString(),
            AiFillDialog::tr("ONNX models (*.onnx);;All files (*)"));
        if (!path.isEmpty())
            lamaPath->setText(path);
    });
    QObject::connect(lamaDownload, &QPushButton::clicked, &dialog, [=, &dialog] {
        const QString path = aifill::defaultLamaModelPath();
        const QString error = downloadModel(&dialog, path);
        if (!error.isEmpty()) {
            QMessageBox::warning(&dialog, AiFillDialog::tr("Download"), error);
            return;
        }
        lamaPath->clear(); // the default location is where it went
        refreshLama();
    });
    refreshLama();
    layout->addWidget(lamaBox);

    // --- online ------------------------------------------------------------
    QGroupBox *cloudBox = new QGroupBox(AiFillDialog::tr("Online image model"), &dialog);
    QFormLayout *cloudForm = new QFormLayout(cloudBox);
    cloudForm->addRow(note(AiFillDialog::tr(
        "An image-edit service that speaks the OpenAI Images API. The part of the picture "
        "around each selected region is uploaded to it, and the service charges for each one. "
        "The key is stored in this program's settings as plain text; leave it empty to use the "
        "OPENAI_API_KEY environment variable instead.")));
    QLineEdit *cloudUrl = new QLineEdit(settings->cloudBaseUrl, &dialog);
    cloudUrl->setPlaceholderText(QString::fromLatin1(aifill::kDefaultCloudBaseUrl));
    cloudForm->addRow(AiFillDialog::tr("Address:"), cloudUrl);
    QLineEdit *cloudModel = new QLineEdit(settings->cloudModel, &dialog);
    cloudModel->setPlaceholderText(QString::fromLatin1(aifill::kDefaultCloudModel));
    cloudForm->addRow(AiFillDialog::tr("Model:"), cloudModel);
    QLineEdit *cloudKey = new QLineEdit(settings->cloudApiKey, &dialog);
    cloudKey->setEchoMode(QLineEdit::Password);
    cloudForm->addRow(AiFillDialog::tr("API key:"), cloudKey);
    layout->addWidget(cloudBox);

    QDialogButtonBox *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(dialog.sizeHint().expandedTo(QSize(620, 0)));

    if (dialog.exec() != QDialog::Accepted)
        return false;

    settings->lamaModelPath = lamaPath->text().trimmed();
    settings->cloudBaseUrl = cloudUrl->text().trimmed();
    settings->cloudModel = cloudModel->text().trimmed();
    settings->cloudApiKey = cloudKey->text().trimmed();
    settings->save();
    return true;
}

} // namespace

AiFillDialog::AiFillDialog(const QByteArray &destJpeg, const jr::Info &dest,
                           const jr::Samples &destRgb, const QByteArray &mask, QWidget *parent)
    : QDialog(parent)
    , m_destJpeg(destJpeg)
    , m_dest(dest)
    , m_destRgb(destRgb)
    , m_mask(mask)
{
    setWindowTitle(tr("AI Fill"));
    m_plan = fill::planFor(m_mask, m_dest);
    m_regions = aifill::regions(m_mask, m_dest);
    m_current = m_destRgb.toImage().copy(0, 0, m_dest.width, m_dest.height);
    m_settings = aifill::Settings::load();
    buildUi();
}

void AiFillDialog::buildUi()
{
    QVBoxLayout *layout = new QVBoxLayout(this);

    QLabel *intro = new QLabel(
        tr("Where the picture data is gone and there is no other copy of the photograph, a "
           "model can make up content that fits what surrounds the gap. That content is "
           "invented, not recovered: it shows what could plausibly have been there, not what "
           "was.\n\n"
           "Only the selected MCUs receive it. They are compressed once, with this file's own "
           "quantization tables, and every other MCU keeps the exact coefficients it has now. "
           "Nothing is changed until you choose Fill MCUs."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    QFormLayout *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    QHBoxLayout *providerRow = new QHBoxLayout;
    m_providerCombo = new QComboBox(this);
    QPushButton *setUp = new QPushButton(tr("Set up…"), this);
    connect(setUp, &QPushButton::clicked, this, &AiFillDialog::onSetUp);
    providerRow->addWidget(m_providerCombo, 1);
    providerRow->addWidget(setUp);
    form->addRow(tr("Model:"), providerRow);

    m_providerNote = new QLabel(this);
    m_providerNote->setWordWrap(true);
    form->addRow(QString(), m_providerNote);

    m_promptEdit = new QLineEdit(this);
    form->addRow(tr("What belongs there:"), m_promptEdit);

    m_uprightCombo = new QComboBox(this);
    m_uprightCombo->addItem(tr("As it is shown"), 0);
    m_uprightCombo->addItem(tr("Turned a quarter clockwise"), 1);
    m_uprightCombo->addItem(tr("Turned upside down"), 2);
    m_uprightCombo->addItem(tr("Turned a quarter counterclockwise"), 3);
    m_uprightCombo->setToolTip(
        tr("Which way up the picture really is. A model that draws subjects is shown each "
           "region turned this way, because it draws a face lying on its side much worse than "
           "one standing up. The file itself is not turned."));
    // The camera's own note of which way it was held, when the file has one.
    switch (exif::orientation(m_destJpeg)) {
    case 6: m_uprightCombo->setCurrentIndex(1); break;
    case 3: m_uprightCombo->setCurrentIndex(2); break;
    case 8: m_uprightCombo->setCurrentIndex(3); break;
    default: break;
    }
    form->addRow(tr("The picture is upright:"), m_uprightCombo);
    layout->addLayout(form);

    // --- the two panels ----------------------------------------------------
    QHBoxLayout *panels = new QHBoxLayout;
    const auto addPanel = [this, panels](const QString &title, QLabel **view) {
        QGroupBox *box = new QGroupBox(title, this);
        QVBoxLayout *boxLayout = new QVBoxLayout(box);
        *view = new QLabel(box);
        (*view)->setAlignment(Qt::AlignCenter);
        (*view)->setMinimumSize(kPanelMinWidth, kPanelMinHeight);
        (*view)->setFrameShape(QFrame::StyledPanel);
        (*view)->setWordWrap(true);
        boxLayout->addWidget(*view);
        panels->addWidget(box, 1);
    };
    addPanel(tr("As it is now"), &m_currentView);
    addPanel(tr("With the fill"), &m_resultView);
    layout->addLayout(panels, 1);

    QHBoxLayout *regionRow = new QHBoxLayout;
    m_previousButton = new QPushButton(tr("Previous region"), this);
    m_nextButton = new QPushButton(tr("Next region"), this);
    m_regionLabel = new QLabel(this);
    connect(m_previousButton, &QPushButton::clicked, this, [this] {
        m_region = qMax(0, m_region - 1);
        showPanels();
        updateButtons();
    });
    connect(m_nextButton, &QPushButton::clicked, this, [this] {
        m_region = qMin(int(m_regions.size()) - 1, m_region + 1);
        showPanels();
        updateButtons();
    });
    regionRow->addWidget(m_previousButton);
    regionRow->addWidget(m_nextButton);
    regionRow->addWidget(m_regionLabel, 1);
    layout->addLayout(regionRow);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_generateButton = m_buttons->addButton(tr("Generate"), QDialogButtonBox::ActionRole);
    m_fillButton = m_buttons->addButton(tr("Fill MCUs"), QDialogButtonBox::AcceptRole);
    connect(m_generateButton, &QPushButton::clicked, this, &AiFillDialog::onGenerate);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &AiFillDialog::onFill);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttons);

    connect(m_providerCombo, &QComboBox::currentIndexChanged, this,
            &AiFillDialog::onProviderChanged);
    reloadProviders();

    const QLocale locale;
    setStatus(tr("%1 MCU(s) selected in %2 region(s), of the %3 in this picture.")
                  .arg(locale.toString(m_plan.mcuCount))
                  .arg(m_regions.size())
                  .arg(locale.toString(m_dest.mcuCount())),
              false);
    showPanels();
    updateButtons();
    resize(sizeHint().expandedTo(QSize(900, 700)));
}

// ---------------------------------------------------------------------------
// Choosing the model
// ---------------------------------------------------------------------------

void AiFillDialog::reloadProviders()
{
    QString wanted = m_settings.provider;
    if (const aifill::Provider *current = currentProvider())
        wanted = current->id();

    m_providers = aifill::providers(m_settings);

    QSignalBlocker block(m_providerCombo);
    m_providerCombo->clear();
    QStandardItemModel *model = qobject_cast<QStandardItemModel *>(m_providerCombo->model());
    int choose = -1;
    for (int i = 0; i < int(m_providers.size()); ++i) {
        const aifill::Provider &provider = *m_providers[size_t(i)];
        QString why;
        const bool usable = provider.available(&why);
        // Every model is listed, usable or not; one that cannot run says why
        // rather than going missing.
        m_providerCombo->addItem(usable ? provider.displayName()
                                        : tr("%1 (not available)").arg(provider.displayName()),
                                 provider.id());
        m_providerCombo->setItemData(i, why, Qt::ToolTipRole);
        if (model && !usable)
            model->item(i)->setEnabled(false);
        if (usable && (choose < 0 || provider.id() == wanted))
            choose = i;
    }
    // With nothing chosen before, start on the one that needs nothing else
    // running, when it is installed.
    if (wanted.isEmpty()) {
        const int local = m_providerCombo->findData(QStringLiteral("lama"));
        if (local >= 0 && m_providers[size_t(local)]->available())
            choose = local;
    }
    m_providerCombo->setCurrentIndex(choose);
    block.unblock();
    onProviderChanged();
}

aifill::Provider *AiFillDialog::currentProvider() const
{
    const int index = m_providerCombo ? m_providerCombo->currentIndex() : -1;
    if (index < 0 || index >= int(m_providers.size()))
        return nullptr;
    return m_providers[size_t(index)].get();
}

void AiFillDialog::onProviderChanged()
{
    refreshNotice();
    const aifill::Provider *provider = currentProvider();
    const bool prompt = provider && provider->usesPrompt();
    m_promptEdit->setEnabled(prompt);
    m_promptEdit->setPlaceholderText(
        prompt ? tr("Optional: a few words, such as \"a man's forehead and dark hair\"")
               : tr("Not used: this model takes no description, only the picture"));
    updateButtons();
}

void AiFillDialog::refreshNotice()
{
    QStringList lines;
    bool warning = false;

    if (const aifill::Provider *provider = currentProvider()) {
        if (provider->sendsPictureOffMachine()) {
            lines << tr("The part of the picture around each selected region (%1 × %1 px) will "
                        "be sent to %2.")
                         .arg(provider->tileSize())
                         .arg(provider->destination());
            warning = true;
        } else {
            lines << tr("The picture stays on this computer.");
        }
    } else {
        lines << tr("No model can run yet. Use Set up to prepare one.");
        warning = true;
    }

    // Why the others are grayed out, where it can be read without hovering.
    for (const auto &provider : m_providers) {
        QString why;
        if (!provider->available(&why))
            lines << tr("%1 is not available: %2").arg(provider->displayName(), why);
    }

    m_providerNote->setStyleSheet(warning ? warningTextStyle() : QString());
    m_providerNote->setText(lines.join(QStringLiteral("\n")));
}

void AiFillDialog::onSetUp()
{
    if (!editSettings(this, &m_settings))
        return;
    reloadProviders();
}

// ---------------------------------------------------------------------------
// Generating
// ---------------------------------------------------------------------------

void AiFillDialog::onGenerate()
{
    aifill::Provider *chosen = currentProvider();
    if (!chosen || !m_plan.isValid())
        return;

    if (chosen->sendsPictureOffMachine()
        && !m_settings.confirmedHosts.contains(chosen->destination())) {
        const auto answer = QMessageBox::question(
            this, tr("Send part of the picture?"),
            tr("This model does not run on this computer. The part of the picture around each "
               "selected region will be uploaded to %1, which may keep it and may charge for "
               "the work.\n\nSend it?")
                .arg(chosen->destination()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
        m_settings.confirmedHosts.append(chosen->destination());
        m_settings.save();
    }

    m_settings.provider = chosen->id();
    m_settings.save();

    // The run gets a provider of its own, so nothing is shared with this
    // thread while it works.
    std::shared_ptr<aifill::Provider> provider;
    for (auto &made : aifill::providers(m_settings)) {
        if (made->id() == chosen->id())
            provider = std::move(made);
    }
    if (!provider)
        return;

    struct Outcome {
        std::optional<aifill::Result> result;
        QString error;
    };

    QProgressDialog progress(tr("Asking the model…"), tr("Cancel"), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    std::atomic<bool> cancelled{false};
    QFutureWatcher<Outcome> watcher;
    connect(&watcher, &QFutureWatcherBase::finished, &progress, &QProgressDialog::reset);
    connect(&progress, &QProgressDialog::canceled, this, [&cancelled] { cancelled = true; });

    const jr::Samples rgb = m_destRgb;
    const QByteArray mask = m_mask;
    const jr::Info info = m_dest;
    const QString prompt = m_promptEdit->isEnabled() ? m_promptEdit->text() : QString();
    const int turns = m_uprightCombo->currentData().toInt();
    QProgressDialog *bar = &progress;
    watcher.setFuture(QtConcurrent::run([=, &cancelled] {
        Outcome outcome;
        outcome.result = aifill::run(
            *provider, rgb, mask, info, prompt, turns,
            [bar](int done, int total) {
                QMetaObject::invokeMethod(
                    bar,
                    [bar, done, total] {
                        // Stays a busy bar for a single window, where there
                        // is no progress to show until it is over.
                        if (total > 1 && done < total) {
                            bar->setMaximum(total);
                            bar->setValue(done);
                            bar->setLabelText(AiFillDialog::tr("Asking the model: window %1 of %2…")
                                                  .arg(done + 1)
                                                  .arg(total));
                        }
                    },
                    Qt::QueuedConnection);
            },
            [&cancelled] { return cancelled.load(); }, &outcome.error);
        return outcome;
    }));
    progress.exec();
    watcher.waitForFinished();
    // Progress posted from the worker may still be queued for a bar that is
    // about to go away.
    QCoreApplication::removePostedEvents(&progress);
    const Outcome outcome = watcher.result();

    if (!outcome.result) {
        if (!cancelled)
            setStatus(outcome.error.isEmpty() ? tr("The model gave no answer.") : outcome.error, true);
        updateButtons();
        return;
    }

    m_result = outcome.result;
    m_usedProvider = provider->displayName();

    QString text = tr("Generated with %1 in %n window(s). Look at each region; ask again if it "
                      "does not convince.",
                      nullptr, m_result->tiles)
                       .arg(m_usedProvider);
    const bool poor = m_result->worstResidual > aifill::kPoorResidual;
    if (poor) {
        text += QLatin1Char(' ')
            + tr("The model redrew the surroundings differently from the original, so the fill "
                 "may not line up at its edges.");
    }
    setStatus(text, poor);
    showPanels();
    updateButtons();
}

// ---------------------------------------------------------------------------
// The panels
// ---------------------------------------------------------------------------

QRect AiFillDialog::viewRect(int index) const
{
    if (index < 0 || index >= m_regions.size())
        return QRect();
    const QRect mcus = m_regions.at(index);
    const QRect pixels(mcus.x() * m_dest.mcuWidth, mcus.y() * m_dest.mcuHeight,
                       mcus.width() * m_dest.mcuWidth, mcus.height() * m_dest.mcuHeight);
    QRect view = pixels.adjusted(-kViewPadding, -kViewPadding, kViewPadding, kViewPadding);
    // A long thin region would otherwise be shown as a ribbon too flat to
    // read; give it height.
    const int wantHeight = qMax(kViewMinSide, view.width() * 2 / 3);
    if (view.height() < wantHeight)
        view.adjust(0, -(wantHeight - view.height()) / 2, 0, (wantHeight - view.height() + 1) / 2);
    if (view.width() < kViewMinSide)
        view.adjust(-(kViewMinSide - view.width()) / 2, 0, (kViewMinSide - view.width() + 1) / 2, 0);
    return view.intersected(QRect(0, 0, m_dest.width, m_dest.height));
}

void AiFillDialog::showPanels()
{
    if (m_regions.isEmpty()) {
        m_currentView->setText(tr("No MCUs are selected. Close this, select the damaged MCUs, "
                                  "and open it again."));
        m_resultView->clear();
        m_regionLabel->clear();
        return;
    }

    const QRect view = viewRect(m_region);
    const QRect mcus = m_regions.at(m_region);
    const QRect outline = QRect(mcus.x() * m_dest.mcuWidth, mcus.y() * m_dest.mcuHeight,
                                mcus.width() * m_dest.mcuWidth, mcus.height() * m_dest.mcuHeight)
                              .intersected(QRect(0, 0, m_dest.width, m_dest.height));

    m_currentView->setPixmap(cropFor(m_current, view, m_currentView->size(), outline));
    if (m_result) {
        // No outline here: the seam it would cover is the thing to look at.
        m_resultView->setPixmap(cropFor(m_result->image, view, m_resultView->size(), QRect()));
    } else {
        m_resultView->setPixmap(QPixmap());
        m_resultView->setText(tr("Choose Generate to see what the model makes of it."));
    }

    m_regionLabel->setText(tr("Region %1 of %2 (%3 × %4 MCUs, at row %5, column %6)")
                               .arg(m_region + 1)
                               .arg(m_regions.size())
                               .arg(mcus.width())
                               .arg(mcus.height())
                               .arg(mcus.y())
                               .arg(mcus.x()));
}

void AiFillDialog::setStatus(const QString &text, bool warning)
{
    m_status->setStyleSheet(warning ? warningTextStyle() : QString());
    m_status->setText(text);
}

void AiFillDialog::updateButtons()
{
    const bool canRun = m_plan.isValid() && currentProvider() != nullptr;
    m_generateButton->setEnabled(canRun);
    m_generateButton->setText(m_result ? tr("Generate again") : tr("Generate"));
    m_generateButton->setToolTip(
        canRun ? QString() : tr("Needs a selection and a model that can run. See Set up."));
    m_fillButton->setEnabled(m_result.has_value());
    m_fillButton->setToolTip(m_result ? QString()
                                      : tr("There is nothing to fill with until Generate has run."));
    m_previousButton->setEnabled(m_region > 0);
    m_nextButton->setEnabled(m_region + 1 < m_regions.size());
    // A default button that is disabled swallows Enter; follow what can be done.
    (m_result ? m_fillButton : m_generateButton)->setDefault(true);
}

void AiFillDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    showPanels();
}

// ---------------------------------------------------------------------------
// Committing
// ---------------------------------------------------------------------------

void AiFillDialog::onFill()
{
    if (!m_plan.isValid() || !m_result)
        return;

    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::optional<jr::Op> op;
    if (const auto aligned = fill::align(m_result->image, m_dest, &error))
        op = fill::build(m_destJpeg, m_dest, *aligned, m_mask, m_plan, QPoint(0, 0), &error);
    QApplication::restoreOverrideCursor();

    if (!op) {
        setStatus(error.isEmpty() ? tr("Those MCUs could not be filled.") : error, true);
        return;
    }
    m_op = *op;
    accept();
}
