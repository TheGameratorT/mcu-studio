// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// mcu-studio-cli: the repair core without a window.
//
// For work that does not need eyes on every file -- sorting a folder of
// rescued photographs, transplanting one donor header onto a hundred files a
// ransomware run left behind, exporting a session's repair, measuring
// performance -- and for scripts and CI.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSysInfo>
#include <QTextStream>
#include <QThread>

#include <cstdio>

#include "Analysis.h"
#include "Bitstream.h"
#include "DonorHeader.h"
#include "Exif.h"
#include "ImageDocument.h"
#include "JpegRepair.h"
#include "JpegStructure.h"
#include "ProjectFile.h"
#include "Report.h"

namespace {

QTextStream &out()
{
    static QTextStream s(stdout);
    return s;
}

QTextStream &err()
{
    static QTextStream s(stderr);
    return s;
}

int fail(const QString &message)
{
    err() << "mcu-studio-cli: " << message << Qt::endl;
    return 1;
}

bool readAll(const QString &path, QByteArray *bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        err() << "mcu-studio-cli: cannot read " << path << ": " << f.errorString() << Qt::endl;
        return false;
    }
    *bytes = f.readAll();
    return true;
}

bool writeAll(const QString &path, const QByteArray &bytes)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) {
        err() << "mcu-studio-cli: cannot write " << path << ": " << f.errorString() << Qt::endl;
        return false;
    }
    return true;
}

void printJson(const QJsonObject &o)
{
    out() << QJsonDocument(o).toJson(QJsonDocument::Indented);
}

QJsonObject triageJson(const QString &path, const analysis::Triage &t)
{
    QJsonObject o;
    o.insert(QStringLiteral("path"), path);
    o.insert(QStringLiteral("verdict"), analysis::verdictName(t.verdict));
    o.insert(QStringLiteral("summary"), t.summary);
    o.insert(QStringLiteral("width"), t.width);
    o.insert(QStringLiteral("height"), t.height);
    o.insert(QStringLiteral("mcus"), t.mcus);
    o.insert(QStringLiteral("decodeErrors"), t.decodeErrors);
    o.insert(QStringLiteral("firstError"), t.firstError);
    o.insert(QStringLiteral("readable"), t.readable);
    return o;
}

// --- commands ----------------------------------------------------------------

int cmdInfo(const QStringList &args, bool json)
{
    if (args.isEmpty())
        return fail(QStringLiteral("info needs a file"));
    QByteArray bytes;
    if (!readAll(args.first(), &bytes))
        return 1;
    const jpegfile::Structure s = jpegfile::walk(bytes);
    const jpegfile::Trailer trailer = jpegfile::findTrailer(bytes, s.imageEnd);
    const auto info = jr::probe(trailer.present() ? bytes.left(trailer.offset) : bytes);
    const analysis::Triage t = analysis::triage(bytes);

    QJsonObject o = triageJson(args.first(), t);
    if (info) {
        o.insert(QStringLiteral("sampling"), info->samplingName());
        o.insert(QStringLiteral("mcuGrid"), QStringLiteral("%1x%2").arg(info->mcusX).arg(info->mcusY));
        o.insert(QStringLiteral("progressive"), info->progressive);
        o.insert(QStringLiteral("scans"), info->scanCount);
        o.insert(QStringLiteral("restartInterval"), info->restartInterval);
        o.insert(QStringLiteral("dcQuant"), QJsonArray{info->dcQuant[0], info->dcQuant[1], info->dcQuant[2]});
    }
    o.insert(QStringLiteral("camera"), exif::cameraModel(bytes));
    if (trailer.present())
        o.insert(QStringLiteral("trailer"), trailer.describe());
    if (!s.problem.isEmpty())
        o.insert(QStringLiteral("structure"), QStringLiteral("this file ") + s.problem);
    QJsonArray embedded;
    for (const jpegfile::Embedded &e : jpegfile::embeddedJpegs(bytes))
        embedded.append(QStringLiteral("%1 %2x%3 at %4").arg(e.origin).arg(e.width).arg(e.height).arg(e.offset));
    if (!embedded.isEmpty())
        o.insert(QStringLiteral("embedded"), embedded);

    if (json) {
        printJson(o);
        return 0;
    }
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
        QString value;
        if (it->isArray()) {
            QStringList parts;
            for (const QJsonValue &v : it->toArray())
                parts << v.toVariant().toString();
            value = parts.join(QStringLiteral(", "));
        } else {
            value = it->toVariant().toString();
        }
        out() << QStringLiteral("%1: %2").arg(it.key(), -16).arg(value) << '\n';
    }
    return 0;
}

int cmdTriage(const QStringList &args, bool json, const QString &csvPath)
{
    if (args.isEmpty())
        return fail(QStringLiteral("triage needs a folder"));
    QStringList paths;
    QDirIterator it(args.first(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = it.next();
        const QString lower = p.toLower();
        if (lower.endsWith(QLatin1String(".jpg")) || lower.endsWith(QLatin1String(".jpeg"))
            || lower.contains(QLatin1String(".jpg.")) || lower.contains(QLatin1String(".jpeg.")))
            paths << p;
    }
    paths.sort();
    QJsonArray rows;
    QByteArray csv = "path,verdict,width,height,mcus,decode_errors,first_error,readable,summary\n";
    QMap<QString, int> counts;
    for (const QString &p : paths) {
        QByteArray bytes;
        if (!readAll(p, &bytes))
            continue;
        const analysis::Triage t = analysis::triage(bytes);
        counts[analysis::verdictName(t.verdict)]++;
        rows.append(triageJson(p, t));
        csv += QStringLiteral("\"%1\",%2,%3,%4,%5,%6,%7,%8,\"%9\"\n")
                   .arg(p, analysis::verdictName(t.verdict))
                   .arg(t.width)
                   .arg(t.height)
                   .arg(t.mcus)
                   .arg(t.decodeErrors)
                   .arg(t.firstError)
                   .arg(t.readable, 0, 'f', 3)
                   .arg(QString(t.summary).replace(QLatin1Char('"'), QStringLiteral("\"\"")))
                   .toUtf8();
        if (!json)
            out() << QStringLiteral("%1  %2  %3").arg(analysis::verdictName(t.verdict), -28).arg(p, t.summary) << '\n';
    }
    if (!csvPath.isEmpty() && !writeAll(csvPath, csv))
        return 1;
    if (json) {
        QJsonObject o;
        o.insert(QStringLiteral("files"), rows);
        printJson(o);
    } else {
        out() << '\n';
        for (auto c = counts.constBegin(); c != counts.constEnd(); ++c)
            out() << QStringLiteral("%1: %2").arg(c.key()).arg(c.value()) << '\n';
    }
    return 0;
}

int cmdApply(const QStringList &args, const QString &output, bool writeReport)
{
    if (args.isEmpty())
        return fail(QStringLiteral("apply needs a project (.mcup)"));
    QString error;
    const auto p = project::read(args.first(), &error);
    if (!p)
        return fail(error);
    ImageDocument doc;
    if (!project::openSource(*p, doc, &error))
        return fail(error);
    if (!p->steps.isEmpty() && !doc.adoptSteps(p->steps, &error))
        return fail(error);
    const QString target = !output.isEmpty() ? output
        : !p->exportPath.isEmpty()           ? p->exportPath
                                             : p->sourcePath + QStringLiteral(".repaired.jpg");
    if (!doc.exportTo(target, &error))
        return fail(error);
    out() << "wrote " << target << '\n';
    if (writeReport) {
        QByteArray written;
        readAll(target, &written);
        if (!report::writeBeside(target, report::build(doc, target, written), &error))
            return fail(error);
        out() << "wrote " << target << ".report.json\n";
    }
    return 0;
}

int cmdResync(const QStringList &args, qint64 mcu, qint64 byteOffset, const QString &output, bool json)
{
    if (args.isEmpty())
        return fail(QStringLiteral("resync needs a file"));
    QByteArray bytes;
    if (!readAll(args.first(), &bytes))
        return 1;
    const bitstream::Map map = bitstream::map(jr::salvageScan(bytes).data);
    if (!map.isValid())
        return fail(map.unsupported);
    qint64 bitPos = -1;
    if (mcu >= 0 && mcu < map.mcus.size())
        bitPos = map.mcus[mcu].bitPos;
    else if (byteOffset >= 0)
        bitPos = byteOffset * 8;
    else if (map.firstAnomaly >= 0)
        // The decoder notices damage a few MCUs after it starts, and the
        // search only looks forward from where it begins.
        bitPos = map.mcus[qMax(0, map.firstAnomaly - 1)].bitPos;
    if (bitPos < 0) {
        out() << "the stream decodes cleanly; nothing to resync\n";
        return 0;
    }
    const auto found = bitstream::searchResync(bytes, bitPos);
    if (found.isEmpty())
        return fail(QStringLiteral("no candidate decodes"));
    QJsonArray rows;
    for (int i = 0; i < found.size() && i < 10; ++i) {
        const bitstream::Candidate &c = found[i];
        QJsonObject o;
        o.insert(QStringLiteral("deltaBits"), c.deltaBits);
        o.insert(QStringLiteral("atByte"), qint64(c.bitPos >> 3));
        o.insert(QStringLiteral("atBit"), int(c.bitPos & 7));
        o.insert(QStringLiteral("cleanMcus"), c.cleanMcus);
        o.insert(QStringLiteral("dcOffset"), c.dcStep);
        o.insert(QStringLiteral("seams"), c.edgeCost);
        rows.append(o);
        if (!json)
            out() << QStringLiteral("%1. %2 %3 bit(s) at byte %4 bit %5: %6 MCUs clean, DC offset %7, seams %8")
                         .arg(i + 1)
                         .arg(c.deltaBits >= 0 ? QStringLiteral("delete") : QStringLiteral("insert"))
                         .arg(std::abs(c.deltaBits))
                         .arg(c.bitPos >> 3)
                         .arg(c.bitPos & 7)
                         .arg(c.cleanMcus)
                         .arg(c.dcStep, 0, 'f', 2)
                         .arg(c.edgeCost, 0, 'f', 2)
                  << '\n';
    }
    if (json) {
        QJsonObject o;
        o.insert(QStringLiteral("candidates"), rows);
        printJson(o);
    }
    if (!output.isEmpty()) {
        const bitstream::Candidate &best = found.first();
        const auto fixed = best.deltaBits >= 0 ? bitstream::deleteBits(bytes, best.bitPos, best.deltaBits)
                                               : bitstream::insertBits(bytes, best.bitPos, -best.deltaBits);
        if (!fixed || !writeAll(output, *fixed))
            return 1;
        out() << "wrote " << output << " with the first candidate applied\n";
    }
    return 0;
}

int cmdSplice(const QStringList &args, const QString &donorPath, qint64 offset, bool keepOwn,
              const QString &output)
{
    if (args.isEmpty() || donorPath.isEmpty())
        return fail(QStringLiteral("splice needs a damaged file (or folder) and --donor"));
    QByteArray donorBytes;
    if (!readAll(donorPath, &donorBytes))
        return 1;
    const donor::Layout layout = donor::scan(donorBytes);
    const QString problem = donor::donorProblem(layout);
    if (!problem.isEmpty())
        return fail(QStringLiteral("the donor ") + problem);
    donor::SpliceOptions options;
    options.keepOwnTables = keepOwn;

    QStringList inputs;
    const QFileInfo first(args.first());
    if (first.isDir()) {
        QDirIterator it(first.absoluteFilePath(), QDir::Files);
        while (it.hasNext())
            inputs << it.next();
        inputs.sort();
        if (output.isEmpty())
            return fail(QStringLiteral("splicing a folder needs -o <folder>"));
        QDir().mkpath(output);
    } else {
        inputs = args;
    }

    int done = 0;
    for (const QString &path : inputs) {
        QByteArray broken;
        if (!readAll(path, &broken))
            continue;
        const qsizetype at = offset >= 0 ? offset
            : jpegfile::findStopDjvuFooter(broken) ? 0x25800
                                                   : 0;
        QString error;
        const auto spliced = donor::splice(donorBytes, layout, broken, at, &error, options);
        if (!spliced) {
            err() << path << ": " << error << Qt::endl;
            continue;
        }
        ImageDocument doc;
        if (!doc.loadReconstructed(path, spliced->bytes, donorPath, at, &error)) {
            err() << path << ": " << error << Qt::endl;
            continue;
        }
        QString target;
        if (first.isDir()) {
            QString base = QFileInfo(path).fileName();
            const qsizetype jpg = base.indexOf(QStringLiteral(".jp"), 0, Qt::CaseInsensitive);
            if (jpg > 0)
                base = base.left(jpg);
            target = QDir(output).filePath(base + QStringLiteral(".jpg"));
        } else {
            target = output.isEmpty() ? path + QStringLiteral(".spliced.jpg") : output;
        }
        if (!doc.exportTo(target, &error)) {
            err() << path << ": " << error << Qt::endl;
            continue;
        }
        ++done;
        out() << path << " -> " << target << " (data from byte " << at << ")\n";
    }
    out() << done << " of " << inputs.size() << " file(s) spliced\n";
    return done > 0 ? 0 : 1;
}

int cmdCarve(const QStringList &args, const QString &output, bool embeddedOnly)
{
    if (args.isEmpty() || output.isEmpty())
        return fail(QStringLiteral("needs a file and -o <folder>"));
    QFile f(args.first());
    if (!f.open(QIODevice::ReadOnly))
        return fail(f.errorString());
    uchar *mapped = f.map(0, f.size());
    const QByteArray bytes = mapped ? QByteArray::fromRawData(reinterpret_cast<const char *>(mapped), f.size())
                                    : f.readAll();
    const QVector<jpegfile::Embedded> found = embeddedOnly ? jpegfile::embeddedJpegs(bytes) : jpegfile::carve(bytes);
    QDir().mkpath(output);
    int i = 0;
    for (const jpegfile::Embedded &e : found) {
        const QString name = QDir(output).filePath(QStringLiteral("%1_%2_%3x%4.jpg")
                                                       .arg(QFileInfo(args.first()).completeBaseName())
                                                       .arg(++i, 4, 10, QLatin1Char('0'))
                                                       .arg(e.width)
                                                       .arg(e.height));
        if (writeAll(name, QByteArray(bytes.constData() + e.offset, e.length)))
            out() << name << "  (" << e.origin << ", " << e.length << " bytes)\n";
    }
    out() << found.size() << " JPEG(s) found\n";
    return 0;
}

int cmdBench(const QStringList &args, int repeat)
{
    if (args.isEmpty())
        return fail(QStringLiteral("bench needs a file"));
    QByteArray bytes;
    if (!readAll(args.first(), &bytes))
        return 1;
    const auto best = [repeat](auto fn) {
        qint64 bestNs = std::numeric_limits<qint64>::max();
        for (int i = 0; i < repeat; ++i) {
            QElapsedTimer t;
            t.start();
            fn();
            bestNs = qMin(bestNs, t.nsecsElapsed());
        }
        return bestNs / 1e6;
    };
    std::shared_ptr<jr::Coefs> coefs;
    const double load = best([&] { coefs = jr::Coefs::load(bytes); });
    if (!coefs)
        return fail(QStringLiteral("cannot read the file"));
    const jr::Info info = coefs->info();
    const QVector<jr::Op> dc = {jr::Op::cdelta(0, 2, jr::Scope::wholeImage()),
                                jr::Op::cdelta(1, 2, jr::Scope::wholeImage()),
                                jr::Op::cdelta(2, 2, jr::Scope::wholeImage())};
    std::shared_ptr<jr::Coefs> work;
    const double clone = best([&] { work = coefs->clone(); });
    const double apply = best([&] { work->apply(dc); });
    const double insert = best([&] { work->apply({jr::Op::insertMcus(3, jr::Scope::runFrom(info.mcusY / 2, 0))}); });
    jr::Samples rgb;
    const double renderFull = best([&] { rgb = work->render(false); });
    const double renderRow = best([&] { work->renderRows(rgb, {info.mcusY / 2}, false); });
    QByteArray mask(info.mcuCount(), '\0');
    for (int m = info.mcuCount() / 2; m < info.mcuCount() / 2 + 64; ++m)
        mask[m] = 1;
    const double preview = best([&] {
        ImageDocument::preview(work, rgb, {jr::Op::cdelta(0, 3, jr::Scope::mask(mask, info.mcusY, info.mcusX))}, nullptr);
    });
    const double write = best([&] { work->write(bytes); });
    const double libjpegDecode = best([&] { jr::decodeRgb(bytes); });
    const double oneShot = best([&] { jr::apply(bytes, dc); });

    out() << QStringLiteral("%1: %2x%3, %4, %5 MCUs\n")
                 .arg(QFileInfo(args.first()).fileName())
                 .arg(info.width)
                 .arg(info.height)
                 .arg(info.samplingName())
                 .arg(info.mcuCount());
    out() << QStringLiteral("machine: %1, %2 thread(s), %3\n")
                 .arg(QSysInfo::prettyProductName())
                 .arg(QThread::idealThreadCount())
                 .arg(QSysInfo::currentCpuArchitecture());
    out() << QStringLiteral("best of %1 run(s), milliseconds:\n").arg(repeat);
    const auto row = [](const char *name, double ms) {
        out() << QStringLiteral("  %1 %2\n").arg(QLatin1String(name), -46).arg(ms, 8, 'f', 1);
    };
    row("load (entropy decode, once per file)", load);
    row("copy coefficients", clone);
    row("DC offset on 3 components, whole image", apply);
    row("insert 3 MCUs mid-image", insert);
    row("decode whole picture (all threads)", renderFull);
    row("decode one MCU row", renderRow);
    row("preview a DC edit on 64 MCUs (copy+edit+decode)", preview);
    row("write JPEG (entropy encode, export only)", write);
    row("libjpeg-turbo full decode, for reference", libjpegDecode);
    row("old pipeline: read+edit+write in one call", oneShot);
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mcu-studio-cli"));
    QCoreApplication::setApplicationVersion(QStringLiteral(MCU_STUDIO_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "MCU Studio's repair core on the command line.\n\n"
        "Commands:\n"
        "  info <file>                      what the file is and what is wrong with it\n"
        "  triage <folder>                  sort a folder of files by what is wrong with each\n"
        "  apply <project.mcup>             export the repair a project file holds\n"
        "  resync <file>                    search for the bit/byte edit that resynchronizes a\n"
        "                                   damaged stream (at --mcu, --byte, or the first error)\n"
        "  splice <file|folder> --donor D   transplant a donor header (STOP/Djvu files: data\n"
        "                                   resumes at 150 KiB unless --offset says otherwise)\n"
        "  carve <file> -o <folder>         every complete JPEG inside any file\n"
        "  extract <file> -o <folder>       the pictures embedded in a JPEG (thumbnail, previews)\n"
        "  bench <file>                     time the engine's stages on a file"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("What to do; see above."));
    QCommandLineOption jsonOpt(QStringLiteral("json"), QStringLiteral("Print JSON."));
    QCommandLineOption outOpt({QStringLiteral("o"), QStringLiteral("output")}, QStringLiteral("Output file or folder."), QStringLiteral("path"));
    QCommandLineOption csvOpt(QStringLiteral("csv"), QStringLiteral("triage: also write CSV."), QStringLiteral("path"));
    QCommandLineOption reportOpt(QStringLiteral("report"), QStringLiteral("apply: write a recovery report beside the output."));
    QCommandLineOption mcuOpt(QStringLiteral("mcu"), QStringLiteral("resync: MCU index where the damage starts."), QStringLiteral("n"));
    QCommandLineOption byteOpt(QStringLiteral("byte"), QStringLiteral("resync: byte offset of an MCU start."), QStringLiteral("n"));
    QCommandLineOption donorOpt(QStringLiteral("donor"), QStringLiteral("splice: the donor JPEG."), QStringLiteral("file"));
    QCommandLineOption offsetOpt(QStringLiteral("offset"), QStringLiteral("splice: where the damaged data resumes."), QStringLiteral("bytes"));
    QCommandLineOption donorTablesOpt(QStringLiteral("donor-tables"), QStringLiteral("splice: use only the donor's tables, not the file's own surviving ones."));
    QCommandLineOption repeatOpt(QStringLiteral("repeat"), QStringLiteral("bench: runs per stage (default 5)."), QStringLiteral("n"), QStringLiteral("5"));
    parser.addOptions({jsonOpt, outOpt, csvOpt, reportOpt, mcuOpt, byteOpt, donorOpt, offsetOpt, donorTablesOpt, repeatOpt});
    parser.process(app);

    QStringList args = parser.positionalArguments();
    if (args.isEmpty())
        parser.showHelp(1);
    const QString command = args.takeFirst();
    const bool json = parser.isSet(jsonOpt);
    const QString output = parser.value(outOpt);

    if (command == QLatin1String("info"))
        return cmdInfo(args, json);
    if (command == QLatin1String("triage"))
        return cmdTriage(args, json, parser.value(csvOpt));
    if (command == QLatin1String("apply"))
        return cmdApply(args, output, parser.isSet(reportOpt));
    if (command == QLatin1String("resync"))
        return cmdResync(args, parser.isSet(mcuOpt) ? parser.value(mcuOpt).toLongLong() : -1,
                         parser.isSet(byteOpt) ? parser.value(byteOpt).toLongLong() : -1, output, json);
    if (command == QLatin1String("splice"))
        return cmdSplice(args, parser.value(donorOpt),
                         parser.isSet(offsetOpt) ? parser.value(offsetOpt).toLongLong() : -1,
                         !parser.isSet(donorTablesOpt), output);
    if (command == QLatin1String("carve"))
        return cmdCarve(args, output, false);
    if (command == QLatin1String("extract"))
        return cmdCarve(args, output, true);
    if (command == QLatin1String("bench"))
        return cmdBench(args, qMax(1, parser.value(repeatOpt).toInt()));
    return fail(QStringLiteral("unknown command \"%1\"; see --help").arg(command));
}
