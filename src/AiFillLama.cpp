// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// The model that runs on this computer: LaMa, through ONNX Runtime.
//
// LaMa fills a hole from what surrounds it and nothing else. It takes no
// instructions and invents no subject, which suits damage well: a strip across
// sand or skin or foliage comes back as more of the same. It is also the only
// choice that keeps the picture on the machine.

#include "AiFill.h"

#include <QCoreApplication>
#include <QFileInfo>

#ifdef MCU_HAVE_ONNX
// ONNX Runtime is opened when the local model is first asked about rather than
// linked, so the program starts without it and says so. The headers are told
// not to look the library up themselves; loadRuntime() hands them its API.
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>

#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#endif

namespace aifill {
namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("aifill", text);
}

// The exported model's input is fixed at this size.
constexpr int kLamaSide = 512;

#ifdef MCU_HAVE_ONNX
// Opens ONNX Runtime once. A failure is not remembered, so installing the
// library while the program runs is enough.
bool loadRuntime(QString *whyNot)
{
    static QMutex mutex;
    static bool loaded = false;
    QMutexLocker lock(&mutex);
    if (loaded)
        return true;

    // The copy the build was pointed at, if any, then the system's: the name
    // its packages install (libonnxruntime.so.1 or .so.1.N) and the one that
    // only comes with the development files. Windows ignores the version.
    std::vector<std::unique_ptr<QLibrary>> candidates;
#ifdef MCU_ONNX_LIBRARY
    candidates.push_back(std::make_unique<QLibrary>(QStringLiteral(MCU_ONNX_LIBRARY)));
#endif
    for (const QString &version : {QStringLiteral("1"),
                                   QStringLiteral("1.%1").arg(ORT_API_VERSION), QString()}) {
        candidates.push_back(
            std::make_unique<QLibrary>(QStringLiteral("onnxruntime"), version));
    }

#ifdef Q_OS_WIN
    // It ships with the program there, so only a damaged installation lacks it.
    QString problem = tr("ONNX Runtime, which the local model needs, is missing from this "
                         "installation. Install MCU Studio again to restore it.");
#else
    QString problem = tr("ONNX Runtime, which the local model needs, is not installed. "
                         "Install it (the onnxruntime package on most systems) to use the "
                         "local model.");
#endif
    for (const auto &library : candidates) {
        using GetApiBase = const OrtApiBase *(ORT_API_CALL *)(void);
        const auto getApiBase = reinterpret_cast<GetApiBase>(library->resolve("OrtGetApiBase"));
        const OrtApiBase *base = getApiBase ? getApiBase() : nullptr;
        if (!base)
            continue;
        // Null when the library is older than the headers this was built with.
        if (const OrtApi *api = base->GetApi(ORT_API_VERSION)) {
            Ort::InitApi(api);
            loaded = true;
            return true;
        }
        problem = tr("The installed ONNX Runtime (%1) is too old for this copy of MCU Studio, "
                     "which needs 1.%2 or newer.")
                      .arg(QString::fromUtf8(base->GetVersionString()))
                      .arg(ORT_API_VERSION);
    }
    if (whyNot)
        *whyNot = problem;
    return false;
}
#endif

class LamaProvider : public Provider
{
public:
    explicit LamaProvider(const Settings &settings)
        : m_modelPath(settings.effectiveLamaModelPath())
        , m_gpuWanted(settings.lamaUseGpu)
    {
    }

    QString id() const override { return QStringLiteral("lama"); }
    QString displayName() const override
    {
        // Once it has run, which processor it ran on is worth knowing.
        if (m_device == Device::Gpu)
            return tr("LaMa (runs on this computer, on the graphics card)");
        if (m_device == Device::Cpu && m_gpuWanted && !m_gpuProblem.isEmpty())
            return tr("LaMa (runs on this computer, on the processor)");
        return tr("LaMa (runs on this computer)");
    }
    bool sendsPictureOffMachine() const override { return false; }
    bool usesPrompt() const override { return false; }
    int tileSize() const override { return kLamaSide; }
    bool regeneratesContext() const override { return false; }

    bool available(QString *whyNot) const override
    {
        if (!localModelSupported(whyNot))
            return false;
        const QFileInfo file(m_modelPath);
        if (!file.isFile() || file.size() == 0) {
            if (whyNot)
                *whyNot = tr("The model file is not installed yet. Use Set up to download it "
                             "(about 200 MB) or to point at a copy you already have.");
            return false;
        }
        return true;
    }

    std::optional<QImage> inpaint(const Request &request, const std::function<bool()> &,
                                  QString *error) override
    {
#ifndef MCU_HAVE_ONNX
        Q_UNUSED(request);
        available(error);
        return std::nullopt;
#else
        if (!loadRuntime(error))
            return std::nullopt;
        try {
            return infer(request);
        } catch (const Ort::Exception &e) {
            if (error)
                *error = tr("The local model failed: %1").arg(QString::fromUtf8(e.what()));
        } catch (const std::exception &e) {
            if (error)
                *error = tr("The local model failed: %1").arg(QString::fromUtf8(e.what()));
        }
        return std::nullopt;
#endif
    }

private:
#ifdef MCU_HAVE_ONNX
    // The graphics card providers this ONNX Runtime was built with that this
    // program knows how to ask for. MIGraphX is the one for AMD cards.
    static bool gpuProviderBuilt()
    {
        const std::vector<std::string> built = Ort::GetAvailableProviders();
        return std::find(built.begin(), built.end(), "MIGraphXExecutionProvider") != built.end();
    }

    void openSession(bool gpu)
    {
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        if (gpu) {
            OrtMIGraphXProviderOptions migraphx{};
            migraphx.device_id = 0;
            migraphx.migraphx_mem_limit = SIZE_MAX;
            options.AppendExecutionProvider_MIGraphX(migraphx);
        }
#ifdef _WIN32
        const std::wstring path = m_modelPath.toStdWString();
#else
        const std::string path = m_modelPath.toStdString();
#endif
        m_session = std::make_unique<Ort::Session>(*m_env, path.c_str(), options);
        m_device = gpu ? Device::Gpu : Device::Cpu;
    }

    // Back to the processor after the graphics card let it down, keeping why.
    void fallBackToCpu(const char *why)
    {
        m_gpuProblem = QString::fromUtf8(why);
        m_session.reset();
        openSession(false);
    }

    void ensureSession()
    {
        if (m_session)
            return;
        if (!m_env)
            m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "mcu-studio");

        // The graphics card when this ONNX Runtime can drive one, and the
        // processor whenever that does not work out: a driver that is missing
        // or a model the card's compiler rejects should cost time, not the fill.
        bool opened = false;
        if (m_gpuWanted && gpuProviderBuilt()) {
            try {
                openSession(true);
                opened = true;
            } catch (const std::exception &e) {
                m_gpuProblem = QString::fromUtf8(e.what());
                m_session.reset();
            }
        }
        if (!opened)
            openSession(false);

        // Two inputs, told apart by their channel count rather than by name,
        // so another export of the same model still loads.
        Ort::AllocatorWithDefaultOptions allocator;
        if (m_session->GetInputCount() != 2 || m_session->GetOutputCount() < 1)
            throw std::runtime_error("the file is not a LaMa inpainting model");
        m_imageName.clear();
        m_maskName.clear();
        for (size_t i = 0; i < 2; ++i) {
            const std::string name = m_session->GetInputNameAllocated(i, allocator).get();
            const std::vector<int64_t> shape =
                m_session->GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() != 4)
                throw std::runtime_error("the file is not a LaMa inpainting model");
            (shape[1] == 1 ? m_maskName : m_imageName) = name;
        }
        if (m_imageName.empty() || m_maskName.empty())
            throw std::runtime_error("the file is not a LaMa inpainting model");
        m_outputName = m_session->GetOutputNameAllocated(0, allocator).get();
    }

    QImage infer(const Request &request)
    {
        // One window at a time; the session is not shared between runs anyway.
        QMutexLocker lock(&m_mutex);
        ensureSession();

        const QSize side(kLamaSide, kLamaSide);
        const QImage image = request.image.size() == side
            ? request.image.convertToFormat(QImage::Format_RGB888)
            : request.image.scaled(side, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                  .convertToFormat(QImage::Format_RGB888);
        const QImage mask = request.mask.size() == side
            ? request.mask.convertToFormat(QImage::Format_Grayscale8)
            : request.mask.scaled(side, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                  .convertToFormat(QImage::Format_Grayscale8);

        const size_t plane = size_t(kLamaSide) * kLamaSide;
        std::vector<float> imageData(plane * 3), maskData(plane);
        for (int y = 0; y < kLamaSide; ++y) {
            const uchar *rgb = image.constScanLine(y);
            const uchar *m = mask.constScanLine(y);
            for (int x = 0; x < kLamaSide; ++x) {
                const size_t at = size_t(y) * kLamaSide + x;
                for (int c = 0; c < 3; ++c)
                    imageData[plane * c + at] = rgb[x * 3 + c] / 255.0f;
                maskData[at] = m[x] ? 1.0f : 0.0f;
            }
        }

        const Ort::MemoryInfo memory =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<int64_t, 4> imageShape{1, 3, kLamaSide, kLamaSide};
        const std::array<int64_t, 4> maskShape{1, 1, kLamaSide, kLamaSide};
        std::array<Ort::Value, 2> inputs{
            Ort::Value::CreateTensor<float>(memory, imageData.data(), imageData.size(),
                                            imageShape.data(), imageShape.size()),
            Ort::Value::CreateTensor<float>(memory, maskData.data(), maskData.size(),
                                            maskShape.data(), maskShape.size())};
        const std::array<const char *, 2> inputNames{m_imageName.c_str(), m_maskName.c_str()};
        const std::array<const char *, 1> outputNames{m_outputName.c_str()};

        const auto runOnce = [&] {
            return m_session->Run(Ort::RunOptions{nullptr}, inputNames.data(), inputs.data(),
                                  inputs.size(), outputNames.data(), outputNames.size());
        };
        std::vector<Ort::Value> outputs;
        if (m_device == Device::Gpu) {
            try {
                outputs = runOnce();
            } catch (const std::exception &e) {
                fallBackToCpu(e.what());
                outputs = runOnce();
            }
        } else {
            outputs = runOnce();
        }
        if (outputs.empty() || !outputs.front().IsTensor())
            throw std::runtime_error("the model returned no picture");

        const std::vector<int64_t> shape =
            outputs.front().GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() != 4 || shape[1] != 3 || shape[2] <= 0 || shape[3] <= 0)
            throw std::runtime_error("the model returned an unexpected shape");
        const int outH = int(shape[2]), outW = int(shape[3]);
        const size_t outPlane = size_t(outW) * outH;
        const float *data = outputs.front().GetTensorData<float>();

        // Exports differ on whether the answer is 0..1 or 0..255.
        const float peak = *std::max_element(data, data + outPlane * 3);
        const float scale = peak <= 1.5f ? 255.0f : 1.0f;

        QImage out(outW, outH, QImage::Format_RGB888);
        for (int y = 0; y < outH; ++y) {
            uchar *line = out.scanLine(y);
            for (int x = 0; x < outW; ++x) {
                const size_t at = size_t(y) * outW + x;
                for (int c = 0; c < 3; ++c) {
                    line[x * 3 + c] =
                        uchar(std::clamp(data[outPlane * c + at] * scale + 0.5f, 0.0f, 255.0f));
                }
            }
        }
        if (out.size() != request.image.size()) {
            out = out.scaled(request.image.size(), Qt::IgnoreAspectRatio,
                             Qt::SmoothTransformation)
                      .convertToFormat(QImage::Format_RGB888);
        }
        return out;
    }

    enum class Device { None, Cpu, Gpu };
    Device m_device = Device::None;
    QMutex m_mutex;
    std::unique_ptr<Ort::Env> m_env;
    std::unique_ptr<Ort::Session> m_session;
    std::string m_imageName, m_maskName, m_outputName;
#else
    enum class Device { None, Cpu, Gpu };
    Device m_device = Device::None;
#endif

    QString m_modelPath;
    bool m_gpuWanted = false;
    QString m_gpuProblem; // why the graphics card was given up on, if it was
};

} // namespace

bool localModelSupported(QString *whyNot)
{
#ifdef MCU_HAVE_ONNX
    return loadRuntime(whyNot);
#else
    if (whyNot)
        *whyNot = tr("This copy of MCU Studio was built without ONNX Runtime, which the "
                     "local model needs.");
    return false;
#endif
}

std::unique_ptr<Provider> makeLamaProvider(const Settings &settings)
{
    return std::make_unique<LamaProvider>(settings);
}

} // namespace aifill
