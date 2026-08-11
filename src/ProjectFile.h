// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// The repair session, written down beside the file being repaired.
//
// A repair is a long search -- try 17 blocks, look, try 18, pull the color
// back, look again -- and until now all of it lived in memory until someone
// remembered to save. That is the wrong way round for this tool in particular:
// the files it works on are the ones that already survived something, the
// sessions run long, and a crash halfway through cost the whole search.
//
// So there is no save. Every change to the recipe is written straight to a
// project file next to the image, and the only thing left to ask for is the
// repaired JPEG itself -- which is an export, not a save, because it is a
// product of the session rather than the session.
//
// What makes this cheap is that ImageDocument already keeps the repair as a
// recipe rather than as pixels: an ordered list of steps replayed from the
// original file. That list is small, it is all there is to lose, and writing
// it down is a couple of kilobytes of JSON. The image itself is never copied
// into the project -- the project names it, along with everything needed to
// re-derive the exact bytes the recipe was built against, down to the donor
// header and splice point of a file that could not be opened on its own.
#pragma once

#include <QString>
#include <QVector>

#include <optional>

#include "ImageDocument.h"
#include "JpegRepair.h"

namespace project {

// Appended to the image's own name, extension and all: photo.jpg is worked on
// beside photo.jpg.mcup. Keeping the full name rather than replacing the
// extension means photo.jpg and photo.png in one folder keep separate
// sessions, and a folder of projects still sorts next to its images.
QString suffix();

// Where the project for `sourcePath` lives.
QString pathForSource(const QString &sourcePath);
bool isProjectPath(const QString &path);

// How a file that would not open on its own was made to open: whose header it
// borrowed, and where its own data was taken to resume. Enough to rebuild the
// same reconstruction byte for byte, provided the donor is still there.
struct Donor {
    QString path;
    qsizetype spliceOffset = 0;
};

struct Project {
    QString sourcePath; // absolute, resolved against the project's own folder
    // What the source measured when the project was written. A mismatch means
    // the recipe was built against different bytes, which is worth saying out
    // loud before the replay fails for reasons nobody could guess at.
    qint64 sourceBytes = 0;
    jr::SalvageMode salvageMode = jr::SalvageMode::Truncate;
    std::optional<Donor> donor;
    // Where the last export went, so Export can go there again without asking.
    // Absolute, and empty until the session has exported once.
    QString exportPath;
    QVector<RepairStep> steps;
};

// Both are atomic: the project is rewritten on every edit, and a half-written
// one after a crash would lose exactly what it exists to protect.
bool write(const QString &projectPath, const Project &project, QString *error);
std::optional<Project> read(const QString &projectPath, QString *error);

} // namespace project
