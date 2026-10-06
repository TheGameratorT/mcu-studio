// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// Filling damaged MCUs with content a model invents from the rest of the
// picture.
//
// Fill from Reference Picture answers lost data with another copy of the
// photograph. Often there is no other copy. What is left then is to make the
// missing part up so that it fits its surroundings, which is what an inpainting
// model does. Nothing produced here is recovered: it is a plausible stand-in,
// and the interface says so wherever it offers it.
//
// This module is the part that does not depend on which model is asked. It
// cuts the selection into windows a model can take, asks for each one, lays
// the answers back under the selection and nowhere else, and returns one
// picture that fill::build can compress into the selected MCUs. The models
// themselves sit behind Provider.
#pragma once

#include <QByteArray>
#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "JpegRepair.h"

namespace aifill {

// One window handed to a model.
struct Request {
    QImage image;   // RGB888
    QImage mask;    // Grayscale8, the same size; 255 where content is wanted
    QString prompt; // optional words about what belongs there; many models ignore it
};

class Provider
{
public:
    virtual ~Provider() = default;

    virtual QString id() const = 0;          // stable, for settings
    virtual QString displayName() const = 0; // for the interface
    // Whether it can run right now. When it cannot, `whyNot` gets a sentence
    // the interface shows next to the disabled choice.
    virtual bool available(QString *whyNot = nullptr) const = 0;
    // True when the window leaves this computer, which the user is told
    // before anything is sent.
    virtual bool sendsPictureOffMachine() const = 0;
    // Where the window goes, for that same notice. Empty for a local model.
    virtual QString destination() const { return QString(); }
    virtual bool usesPrompt() const = 0;
    // The side of the square window it works best on, in pixels.
    virtual int tileSize() const = 0;
    // True when the answer is a fresh rendering of the whole window rather
    // than the window with the hole filled, so it has to be laid back over the
    // original and matched to it before any of it is used.
    virtual bool regeneratesContext() const = 0;

    // The window with the masked part filled, the same size as request.image.
    // Called off the main thread. `cancelled` is polled where that is possible.
    virtual std::optional<QImage> inpaint(const Request &request,
                                          const std::function<bool()> &cancelled,
                                          QString *error) = 0;
};

// A window and the MCUs it is responsible for.
struct Tile {
    QRect window;        // in picture pixels, inside the picture
    QVector<int> mcus;   // scan-order indices of the MCUs this window fills
    // The size the window is shown to the model at. Larger than the window
    // when a thin region is magnified for a model that works on a coarse grid.
    QSize modelSize;
};

// Windows covering every MCU set in `mcuMask` (info.mcusY * info.mcusX bytes,
// row-major). Each MCU belongs to exactly one window and sits at least a
// quarter of `tileSize` from its edge wherever the picture allows, so the
// model has real content around what it is asked to fill.
QVector<Tile> planTiles(const QByteArray &mcuMask, const jr::Info &info, int tileSize);

// The windows a run over `mcuMask` uses with a model that works on windows of
// `tileSize`. With `magnify`, a region much thinner than the window gets a
// smaller window shown enlarged: models that redraw through a latent grid of
// 8-pixel cells have only two cells to work with across a 16-pixel MCU row,
// and what they draw there neither holds detail nor meets its surroundings.
QVector<Tile> planJob(const QByteArray &mcuMask, const jr::Info &info, int tileSize, bool magnify);

// The separate regions the mask holds, each as its bounding box in MCUs, in
// scan order of their first MCU. MCUs touching side or corner are one region.
QVector<QRect> regions(const QByteArray &mcuMask, const jr::Info &info);

// How an answer that redrew the whole window was fitted back over the
// original.
struct Registration {
    int dx = 0, dy = 0;            // where the answer had drifted to, in pixels
    double gain[3] = {1, 1, 1};    // per channel, answer -> original
    double offset[3] = {0, 0, 0};
    double residual = 0;           // mean absolute difference left, 0..255
};

// Fits `answer` to `original` using only pixels outside `mask` (Grayscale8,
// non-zero where content was wanted) and returns the answer moved and
// recolored to match. All three are the same size.
QImage registerAnswer(const QImage &original, const QImage &answer, const QImage &mask,
                      Registration *fit = nullptr);

// Carries the difference between `original` and `answer` at the hole's edge
// smoothly across the hole and adds it to the answer there, so the fill meets
// the real picture at the same brightness and color all the way round. For
// answers that redrew the surroundings too: how they differ from the real
// ones just outside the hole is how the fill is off just inside it. All three
// are the same size; the result differs from `answer` only under `mask`.
QImage blendSeams(const QImage &original, const QImage &answer, const QImage &mask);

struct Result {
    // The whole picture, info.width x info.height, RGB888. Identical to the
    // input outside the selected MCUs.
    QImage image;
    int tiles = 0;
    // The worst Registration::residual over the windows, for providers that
    // needed one. Above kPoorResidual the answer did not sit well over the
    // original and the seams are likely to show.
    double worstResidual = 0;
};

constexpr double kPoorResidual = 14.0;

using Progress = std::function<void(int done, int total)>;

// Runs `provider` over the selection. `rgb` is the current render.
//
// `quarterTurns` is how many quarter turns clockwise stand the picture
// upright (0 to 3). Each window is shown to the model turned that way and its
// answer turned back: a model that draws subjects has seen far fewer faces
// lying on their side than standing up, and draws them accordingly.
std::optional<Result> run(Provider &provider, const jr::Samples &rgb, const QByteArray &mcuMask,
                          const jr::Info &info, const QString &prompt, int quarterTurns,
                          const Progress &progress, const std::function<bool()> &cancelled,
                          QString *error);

// ---------------------------------------------------------------------------
// The providers this build knows about
// ---------------------------------------------------------------------------

// Stored with QSettings under "aifill/".
struct Settings {
    QString provider;       // Provider::id() last used
    QString lamaModelPath;  // empty: the default location
    // Off unless asked for: on the one card tried (MIGraphX, Radeon RX 7800 XT)
    // preparing the model for the card had not finished after fifteen minutes.
    bool lamaUseGpu = false;
    QString cloudBaseUrl;   // empty: kDefaultCloudBaseUrl
    QString cloudModel;     // empty: kDefaultCloudModel
    QString cloudApiKey;    // empty: the OPENAI_API_KEY environment variable
    QStringList confirmedHosts; // hosts the user agreed to send pictures to

    static Settings load();
    void save() const;

    QString effectiveBaseUrl() const;
    QString effectiveModel() const;
    QString effectiveApiKey() const;
    QString effectiveLamaModelPath() const;
};

extern const char *const kDefaultCloudBaseUrl;
extern const char *const kDefaultCloudModel;
// Where the local model is fetched from when the user asks for it, and what
// the file must hash to.
extern const char *const kLamaModelUrl;
extern const char *const kLamaModelSha256;
// Whether this build can run the local model at all.
bool localModelSupported();
QString defaultLamaModelPath();

// Every provider, available or not, in the order the interface lists them.
std::vector<std::unique_ptr<Provider>> providers(const Settings &settings);

std::unique_ptr<Provider> makeLamaProvider(const Settings &settings);
std::unique_ptr<Provider> makeCloudProvider(const Settings &settings);

} // namespace aifill
