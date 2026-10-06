// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// The model reached over HTTP: an online image-edit service speaking the
// OpenAI Images API.
//
// Online models are far better than a local one at inventing something specific
// (the missing part of a face, say) and they pay for it in two ways this
// program has to deal with. The window leaves the computer, which the user is
// told before it happens. And the answer is a fresh rendering of the whole
// window, not the window with the hole filled, so it comes back slightly moved
// and slightly recolored; aifill::run fits it back over the original before
// taking the part under the selection.

#include "AiFill.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace aifill {
namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("aifill", text);
}

// The one size every model behind this API accepts.
constexpr int kCloudSide = 1024;
// Generation takes a while; this is only there so a dead connection ends.
constexpr int kTimeoutMs = 5 * 60 * 1000;

const char *const kDefaultPrompt =
    "This photograph has a damaged area, marked by the mask. Restore only that area so it "
    "continues the surrounding photograph naturally, with the same subject, lighting, focus, "
    "grain and colors. Do not change anything outside the mask. Do not add text, borders or "
    "new objects.";

QByteArray pngOf(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

void addField(QHttpMultiPart *form, const char *name, const QByteArray &value)
{
    QHttpPart part;
    part.setHeader(QNetworkRequest::ContentDispositionHeader,
                   QStringLiteral("form-data; name=\"%1\"").arg(QLatin1String(name)));
    part.setBody(value);
    form->append(part);
}

void addFile(QHttpMultiPart *form, const char *name, const char *fileName, const QByteArray &png)
{
    QHttpPart part;
    part.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("image/png"));
    part.setHeader(QNetworkRequest::ContentDispositionHeader,
                   QStringLiteral("form-data; name=\"%1\"; filename=\"%2\"")
                       .arg(QLatin1String(name), QLatin1String(fileName)));
    part.setBody(png);
    form->append(part);
}

struct Reply {
    bool ok = false;
    int status = 0;
    QByteArray body;
    QString transportError;
};

// Posts and waits, on the calling thread, giving up when `cancelled` says so.
// Everything network lives and dies inside the call.
template <typename Post>
Reply postAndWait(const Post &post, const std::function<bool()> &cancelled)
{
    QNetworkAccessManager manager;
    QNetworkReply *reply = post(manager);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer poll;
    poll.setInterval(150);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (cancelled && cancelled())
            reply->abort();
    });
    poll.start();
    loop.exec();
    poll.stop();

    Reply out;
    out.body = reply->readAll();
    out.ok = reply->error() == QNetworkReply::NoError;
    out.transportError = reply->errorString();
    out.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    delete reply;
    return out;
}

class CloudProvider : public Provider
{
public:
    explicit CloudProvider(const Settings &settings)
        : m_baseUrl(settings.effectiveBaseUrl())
        , m_model(settings.effectiveModel())
        , m_apiKey(settings.effectiveApiKey())
    {
    }

    QString id() const override { return QStringLiteral("cloud"); }
    QString displayName() const override
    {
        return tr("Online image model (%1 at %2)").arg(m_model, destination());
    }
    bool sendsPictureOffMachine() const override { return true; }
    QString destination() const override { return QUrl(m_baseUrl).host(); }
    bool usesPrompt() const override { return true; }
    int tileSize() const override { return kCloudSide; }
    bool regeneratesContext() const override { return true; }

    bool available(QString *whyNot) const override
    {
        if (!QUrl(m_baseUrl).isValid() || destination().isEmpty()) {
            if (whyNot)
                *whyNot = tr("The service address is not a valid URL. Use Set up to correct it.");
            return false;
        }
        if (m_apiKey.isEmpty()) {
            if (whyNot)
                *whyNot = tr("No API key is set. Use Set up to enter one, or set the "
                             "OPENAI_API_KEY environment variable.");
            return false;
        }
        return true;
    }

    std::optional<QImage> inpaint(const Request &request, const std::function<bool()> &cancelled,
                                  QString *error) override
    {
        const auto fail = [error](const QString &message) {
            if (error)
                *error = message;
            return std::nullopt;
        };
        if (!available(error))
            return std::nullopt;

        const QSize side(kCloudSide, kCloudSide);
        const QImage image =
            request.image.scaled(side, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .convertToFormat(QImage::Format_RGBA8888);
        // The service edits where the mask is fully transparent.
        const QImage hole = request.mask.convertToFormat(QImage::Format_Grayscale8)
                                .scaled(side, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        QImage mask = image;
        for (int y = 0; y < kCloudSide; ++y) {
            const uchar *h = hole.constScanLine(y);
            uchar *line = mask.scanLine(y);
            for (int x = 0; x < kCloudSide; ++x) {
                if (h[x])
                    line[x * 4 + 0] = line[x * 4 + 1] = line[x * 4 + 2] = line[x * 4 + 3] = 0;
            }
        }

        QString prompt = QString::fromLatin1(kDefaultPrompt);
        if (!request.prompt.trimmed().isEmpty())
            prompt += QStringLiteral(" What belongs there: ") + request.prompt.trimmed();

        const QString url = m_baseUrl;
        const QByteArray key = m_apiKey.toUtf8();
        const QByteArray model = m_model.toUtf8();
        const QByteArray imagePng = pngOf(image), maskPng = pngOf(mask);
        const Reply reply = postAndWait(
            [&](QNetworkAccessManager &manager) {
                QNetworkRequest post(QUrl(url + QStringLiteral("/images/edits")));
                post.setRawHeader("Authorization", "Bearer " + key);
                post.setTransferTimeout(kTimeoutMs);

                QHttpMultiPart *form = new QHttpMultiPart(QHttpMultiPart::FormDataType);
                addField(form, "model", model);
                addField(form, "prompt", prompt.toUtf8());
                addField(form, "size", QByteArrayLiteral("1024x1024"));
                addField(form, "n", QByteArrayLiteral("1"));
                addFile(form, "image", "image.png", imagePng);
                addFile(form, "mask", "mask.png", maskPng);
                QNetworkReply *sent = manager.post(post, form);
                form->setParent(sent);
                return sent;
            },
            cancelled);

        if (cancelled && cancelled())
            return fail(tr("Cancelled."));

        const QJsonObject json = QJsonDocument::fromJson(reply.body).object();
        if (!reply.ok) {
            // The service's own words when it has any; they name the problem
            // (a bad key, an unknown model, a refused picture) better than the
            // transport does.
            const QString said = json.value(QStringLiteral("error"))
                                     .toObject()
                                     .value(QStringLiteral("message"))
                                     .toString();
            if (!said.isEmpty()) {
                return fail(tr("%1 answered (HTTP %2): %3")
                                .arg(destination())
                                .arg(reply.status)
                                .arg(said));
            }
            return fail(tr("The request to %1 failed: %2").arg(destination(), reply.transportError));
        }

        const QString encoded = json.value(QStringLiteral("data"))
                                    .toArray()
                                    .at(0)
                                    .toObject()
                                    .value(QStringLiteral("b64_json"))
                                    .toString();
        QImage answer;
        if (encoded.isEmpty() || !answer.loadFromData(QByteArray::fromBase64(encoded.toLatin1())))
            return fail(tr("%1 answered without a picture.").arg(destination()));

        return answer.convertToFormat(QImage::Format_RGB888)
            .scaled(request.image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_RGB888);
    }

private:
    QString m_baseUrl;
    QString m_model;
    QString m_apiKey;
};

} // namespace

std::unique_ptr<Provider> makeCloudProvider(const Settings &settings)
{
    return std::make_unique<CloudProvider>(settings);
}

} // namespace aifill
