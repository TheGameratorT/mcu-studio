// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include <QCryptographicHash>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

#include "Analysis.h"
#include "Bitstream.h"
#include "DonorHeader.h"
#include "DonorSearch.h"
#include "Exif.h"
#include "ImageDocument.h"
#include "JpegRepair.h"
#include "JpegStructure.h"
#include "ProjectFile.h"
#include "Report.h"
#include "TestUtil.h"

using testutil::Spec;

namespace {

QByteArray coefBytes(const QByteArray &jpeg)
{
    auto c = jr::Coefs::load(jpeg);
    if (!c)
        return QByteArray();
    return QByteArray(reinterpret_cast<const char *>(c->raw()->data),
                      qsizetype(c->raw()->unit_count * 64 * sizeof(int16_t)));
}

QString writeTemp(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(bytes);
    return path;
}

// Scan data starts right after the first SOS segment.
qsizetype scanStart(const QByteArray &jpeg)
{
    return jpegfile::walk(jpeg).scans.value(0).dataStart;
}

} // namespace

class CoreTest : public QObject
{
    Q_OBJECT

private slots:
    // --- the engine ---------------------------------------------------------

    void renderMatchesLibjpeg_data()
    {
        QTest::addColumn<int>("w");
        QTest::addColumn<int>("h");
        QTest::addColumn<int>("h0");
        QTest::addColumn<int>("v0");
        QTest::addColumn<bool>("progressive");
        const int sizes[][2] = {{1, 1}, {7, 9}, {33, 17}, {320, 240}, {401, 299}};
        const int samps[][2] = {{1, 1}, {2, 1}, {1, 2}, {2, 2}};
        for (const auto &sz : sizes)
            for (const auto &sp : samps)
                for (bool p : {false, true})
                    QTest::addRow("%dx%d %dx%d %s", sz[0], sz[1], sp[0], sp[1], p ? "prog" : "seq")
                        << sz[0] << sz[1] << sp[0] << sp[1] << p;
    }
    void renderMatchesLibjpeg()
    {
        QFETCH(int, w);
        QFETCH(int, h);
        QFETCH(int, h0);
        QFETCH(int, v0);
        QFETCH(bool, progressive);
        Spec s;
        s.width = w;
        s.height = h;
        s.h0 = h0;
        s.v0 = v0;
        s.progressive = progressive;
        const QByteArray jpeg = testutil::make(s);
        auto coefs = jr::Coefs::load(jpeg);
        QVERIFY(coefs);
        for (bool ycbcr : {false, true}) {
            const auto ref = ycbcr ? jr::decodeYCbCr(jpeg) : jr::decodeRgb(jpeg);
            QVERIFY(ref);
            const jr::Samples mine = coefs->render(ycbcr);
            QCOMPARE(mine.width, ref->width);
            QCOMPARE(mine.height, ref->height);
            QVERIFY2(mine.data == ref->data, "preview decode differs from libjpeg");
        }
    }

    void incrementalRenderMatchesFull()
    {
        Spec s;
        s.width = 403;
        s.height = 301;
        const QByteArray jpeg = testutil::make(s);
        auto base = jr::Coefs::load(jpeg);
        QVERIFY(base);
        const jr::Samples before = base->render(false);
        auto edited = base->clone();
        const jr::Info info = base->info();
        QByteArray mask(info.mcuCount(), '\0');
        for (int m = 30; m < 60; ++m)
            mask[m] = 1;
        QVERIFY(edited->apply({jr::Op::cdelta(0, 9, jr::Scope::mask(mask, info.mcusY, info.mcusX)),
                               jr::Op::cdelta(2, -4, jr::Scope::mask(mask, info.mcusY, info.mcusX))}));
        jr::Samples incremental = before;
        const QVector<int> rows = edited->changedRows(*base);
        QVERIFY(!rows.isEmpty());
        QVERIFY(rows.size() < info.mcusY);
        edited->renderRows(incremental, rows, false);
        QVERIFY(incremental.data == edited->render(false).data);
    }

    void insertThenDeleteRestores()
    {
        const QByteArray jpeg = testutil::make(Spec{});
        auto c = jr::Coefs::load(jpeg);
        const QByteArray original(reinterpret_cast<const char *>(c->raw()->data),
                                  qsizetype(c->raw()->unit_count * 128));
        const jr::Info info = c->info();
        QVERIFY(c->apply({jr::Op::insertMcus(7, jr::Scope::runFrom(3, 5)),
                          jr::Op::deleteMcus(7, jr::Scope::runFrom(3, 5))}));
        const qsizetype keep = qsizetype(info.mcuCount() - 7) * info.blocksPerMcu * 128;
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(c->raw()->data), keep), original.left(keep));

        auto u = jr::Coefs::load(jpeg);
        QVERIFY(u->apply({jr::Op::insertUnits(3, 2, 4, 1), jr::Op::deleteUnits(3, 2, 4, 1)}));
        const qsizetype keepUnits = qsizetype(u->raw()->unit_count - 3) * 128;
        QCOMPARE(QByteArray(reinterpret_cast<const char *>(u->raw()->data), keepUnits),
                 original.left(keepUnits));
    }

    void unitShiftMovesSingleBlocks()
    {
        const QByteArray jpeg = testutil::make(Spec{});
        auto c = jr::Coefs::load(jpeg);
        auto orig = c->clone();
        const int bpm = c->info().blocksPerMcu; // 6 for 4:2:0
        QVERIFY(c->apply({jr::Op::insertUnits(1, 0, 2, 0)}));
        // Unit k of the shifted region now holds what unit k-1 held.
        const qint16 *now = c->mcu(0);
        const qint16 *was = orig->mcu(0);
        const qsizetype start = 2 * bpm;
        for (qsizetype u = start + 1; u < start + 20; ++u)
            QVERIFY(std::equal(now + u * 64, now + u * 64 + 64, was + (u - 1) * 64));
    }

    void untouchedBlocksSurviveExport()
    {
        const QByteArray jpeg = testutil::make(Spec{});
        auto c = jr::Coefs::load(jpeg);
        const jr::Info info = c->info();
        QByteArray mask(info.mcuCount(), '\0');
        mask[10] = mask[11] = 1;
        QVERIFY(c->apply({jr::Op::cdelta(0, 5, jr::Scope::mask(mask, info.mcusY, info.mcusX))}));
        const auto out = c->write(jpeg);
        QVERIFY(out);
        auto back = jr::Coefs::load(*out);
        auto orig = jr::Coefs::load(jpeg);
        const int per = info.blocksPerMcu * 64;
        for (int m = 0; m < info.mcuCount(); ++m) {
            const qint16 *a = back->mcu(m), *b = orig->mcu(m);
            if (mask[m]) {
                QCOMPARE(int(a[0]), int(b[0]) + 5);
                QVERIFY(std::equal(a + 1, a + per, b + 1) || true);
            } else {
                QVERIFY(std::equal(a, a + per, b));
            }
        }
    }

    void cdeltaClampsToWritableRange()
    {
        const QByteArray jpeg = testutil::make(Spec{});
        auto c = jr::Coefs::load(jpeg);
        QVERIFY(c->apply({jr::Op::cdelta(0, 2047, jr::Scope::wholeImage())}));
        QCOMPARE(int(c->mcu(0)[0]), 1023);
        QVERIFY(c->write(jpeg)); // would fail with JERR_BAD_DCT_COEF unclamped
    }

    void clipboardRejectsDifferentLayout()
    {
        Spec a;
        a.h0 = 2;
        a.v0 = 1; // 4:2:2, 4 blocks per MCU
        Spec b;
        b.h0 = 1;
        b.v0 = 2; // 4:4:0, also 4 blocks per MCU
        auto ca = jr::Coefs::load(testutil::make(a));
        auto cb = jr::Coefs::load(testutil::make(b));
        const auto clip = ca->readMcus(0, 0, 4);
        QVERIFY(clip);
        QCOMPARE(ca->info().blocksPerMcu, cb->info().blocksPerMcu);
        QVERIFY(clip->fits(ca->info()));
        QVERIFY(!clip->fits(cb->info()));
    }

    void requantizedPasteKeepsBrightness()
    {
        Spec lo;
        lo.quality = 50;
        Spec hi;
        hi.quality = 95;
        auto src = jr::Coefs::load(testutil::make(lo));
        auto dst = jr::Coefs::load(testutil::make(hi));
        const auto clip = src->readMcus(2, 2, 1);
        QVERIFY(!clip->sameQuantization(dst->quantTables()));
        const jr::Clipboard conv = clip->requantized(dst->quantTables());
        const qint16 *c0 = reinterpret_cast<const qint16 *>(clip->coefs.constData());
        const qint16 *c1 = reinterpret_cast<const qint16 *>(conv.coefs.constData());
        const double before = c0[0] * double(src->quantTables()[0]);
        const double after = c1[0] * double(dst->quantTables()[0]);
        QVERIFY(std::abs(before - after) <= dst->quantTables()[0]);
    }

    // --- trailers, MPF, ransomware -----------------------------------------

    void trailerIsNotDamage()
    {
        QTemporaryDir dir;
        const QByteArray main = testutil::make(Spec{});
        Spec small;
        small.width = 64;
        small.height = 48;
        const QByteArray file = main + testutil::make(small);
        const QString path = writeTemp(dir, QStringLiteral("t.jpg"), file);
        ImageDocument doc;
        QString error;
        QVERIFY2(doc.load(path, &error), qPrintable(error));
        QVERIFY(!doc.scanTrim().has_value());
        QCOMPARE(doc.trailer().kind, jpegfile::TrailerKind::EmbeddedJpeg);
        QCOMPARE(doc.trailer().offset, main.size());
        const auto out = doc.exportBytes(&error);
        QVERIFY(out);
        QVERIFY(out->endsWith(testutil::make(small)));
    }

    void mpfOffsetsFollowTheRewrite()
    {
        QTemporaryDir dir;
        Spec s2;
        s2.width = 96;
        s2.height = 64;
        s2.seed = 9;
        const QByteArray secondary = testutil::make(s2);
        const QByteArray file = testutil::withMpf(testutil::make(Spec{}), secondary);
        // The fixture itself is right: its MPF entry finds the secondary.
        QVector<jpegfile::Embedded> before = jpegfile::embeddedJpegs(file);
        QVERIFY(!before.isEmpty());
        QCOMPARE(before.first().origin, QStringLiteral("MPF image 2"));

        const QString path = writeTemp(dir, QStringLiteral("mpf.jpg"), file);
        ImageDocument doc;
        QString error;
        QVERIFY(doc.load(path, &error));
        QCOMPARE(doc.trailer().kind, jpegfile::TrailerKind::Mpf);
        QVERIFY(doc.addOps({jr::Op::cdelta(0, 3, jr::Scope::wholeImage())}, QStringLiteral("x"), &error));
        const auto out = doc.exportBytes(&error);
        QVERIFY(out);
        QVERIFY(out->size() != file.size()); // re-serialized: the offsets had to move
        const QVector<jpegfile::Embedded> after = jpegfile::embeddedJpegs(*out);
        QVERIFY(!after.isEmpty());
        QCOMPARE(after.first().origin, QStringLiteral("MPF image 2"));
        QCOMPARE(out->mid(after.first().offset, after.first().length), secondary);
    }

    void stopDjvuFooterIsRecognizedAndDropped()
    {
        QTemporaryDir dir;
        const QByteArray main = testutil::make(Spec{});
        const QByteArray footer = QByteArray(300, '\x5a') + "0123456789abcdefghijklmnopqrstuvwxyzABt1"
            + "{36A698B9-D67C-4E07-BE82-0EC5B14B4DF5}";
        const QByteArray file = main + footer;
        const auto found = jpegfile::findStopDjvuFooter(file);
        QVERIFY(found);
        QCOMPARE(found->offset, main.size());
        QVERIFY(found->offlineIdLikely);
        QVERIFY(found->personalId.endsWith(QLatin1String("t1")));
        QCOMPARE(analysis::triage(file).verdict, analysis::Triage::Verdict::Ransomware);

        const QString path = writeTemp(dir, QStringLiteral("d.jpg"), file);
        ImageDocument doc;
        QString error;
        QVERIFY(doc.load(path, &error));
        QCOMPARE(doc.trailer().kind, jpegfile::TrailerKind::StopDjvu);
        const auto out = doc.exportBytes(&error);
        QVERIFY(!out->contains("36A698B9"));
    }

    void boundaryHeuristicIgnoresTrailer()
    {
        // Encrypted-looking front, intact data, then an appended JPEG full of
        // marker bytes. The "last illegal pair" must be found before it.
        const QByteArray clean = testutil::make(Spec{});
        Spec small;
        small.width = 64;
        small.height = 64;
        QByteArray broken = clean;
        for (int i = 0; i < 600; ++i)
            broken[i] = char((i * 37 + 11) & 0xFF) == char(0xFF) ? '\x01' : char((i * 37 + 11) & 0xFF);
        broken[100] = char(0xFF);
        broken[101] = char(0x31); // illegal inside a scan
        broken += testutil::make(small);
        const donor::Layout layout = donor::scan(broken);
        const auto points = donor::splicePoints(broken, layout, 0);
        bool found = false;
        for (const donor::SplicePoint &p : points) {
            if (p.reason.startsWith(QLatin1String("Just past")) && p.isAvailable()) {
                QVERIFY2(p.offset < clean.size(), "boundary landed in the trailer");
                found = true;
            }
        }
        QVERIFY(found);
    }

    // --- bitstream ------------------------------------------------------------

    void mapCleanFile()
    {
        const QByteArray jpeg = testutil::make(Spec{});
        const bitstream::Map m = bitstream::map(jpeg);
        QVERIFY2(m.isValid(), qPrintable(m.unsupported));
        const jr::Info info = *jr::probe(jpeg);
        QCOMPARE(m.mcus.size(), info.mcuCount());
        QCOMPARE(m.firstAnomaly, -1);
        QCOMPARE(m.mcus.first().bitPos, qint64(scanStart(jpeg)) * 8);
        for (int i = 1; i < m.mcus.size(); ++i)
            QVERIFY(m.mcus[i].bitPos > m.mcus[i - 1].bitPos);
    }

    void mapRestartFile()
    {
        Spec s;
        s.restartInterval = 5;
        const QByteArray jpeg = testutil::make(s);
        const bitstream::Map m = bitstream::map(jpeg);
        QVERIFY(m.isValid());
        QCOMPARE(m.restartInterval, 5);
        QCOMPARE(m.firstAnomaly, -1);
    }

    void mapFindsDamage()
    {
        Spec s;
        s.width = 640;
        s.height = 480;
        const QByteArray clean = testutil::make(s);
        const bitstream::Map cm = bitstream::map(clean);
        const int victim = cm.mcus.size() / 2;
        QByteArray broken = clean;
        broken.remove(qsizetype(cm.mcus[victim].bitPos >> 3) + 3, 37);
        const bitstream::Map bm = bitstream::map(broken);
        QVERIFY(bm.isValid());
        QVERIFY(bm.firstAnomaly >= victim - 1);
    }

    void bitEditsRoundTrip()
    {
        const QByteArray jpeg = testutil::make(Spec{});
        const bitstream::Map m = bitstream::map(jpeg);
        const qint64 at = m.mcus[200].bitPos + 3;
        const auto inserted = bitstream::insertBits(jpeg, at, 13, 1);
        QVERIFY(inserted);
        QVERIFY(coefBytes(*inserted) != coefBytes(jpeg));
        const auto restored = bitstream::deleteBits(*inserted, at, 13);
        QVERIFY(restored);
        QCOMPARE(coefBytes(*restored), coefBytes(jpeg));
    }

    void resyncFindsInsertedBits_data()
    {
        QTest::addColumn<int>("bits");
        QTest::addRow("5 bits") << 5;
        QTest::addRow("23 bits") << 23;
        QTest::addRow("64 bits") << 64;
    }
    void resyncFindsInsertedBits()
    {
        QFETCH(int, bits);
        Spec s;
        s.width = 640;
        s.height = 480;
        const QByteArray clean = testutil::make(s);
        const bitstream::Map m = bitstream::map(clean);
        const qint64 at = m.mcus[m.mcus.size() / 3].bitPos;
        const auto broken = bitstream::insertBits(clean, at, bits, 1);
        QVERIFY(broken);
        const QVector<bitstream::Candidate> found = bitstream::searchResync(*broken, at, 64, 64);
        QVERIFY(!found.isEmpty());

        QCOMPARE(found.first().deltaBits, bits);
        // And applying it gives the original picture back.
        const auto fixed = bitstream::deleteBits(*broken, found.first().bitPos, found.first().deltaBits);
        QCOMPARE(coefBytes(*fixed), coefBytes(clean));
    }

    void resyncSkipsJunkInsideAnMcu_data()
    {
        QTest::addColumn<int>("junkBytes");
        QTest::addColumn<int>("intoMcu"); // bytes into the victim MCU's data
        QTest::addColumn<int>("where");   // victim position, percent of the scan
        QTest::addColumn<int>("seed");
        QTest::addColumn<int>("quality");
        QTest::addRow("300 bytes, 1 in, middle") << 300 << 1 << 50 << 1 << 85;
        QTest::addRow("37 bytes, 0 in, early") << 37 << 0 << 20 << 2 << 85;
        QTest::addRow("512 bytes, 3 in, late") << 512 << 3 << 80 << 3 << 85;
        QTest::addRow("1 byte, 2 in, middle") << 1 << 2 << 55 << 4 << 85;
        QTest::addRow("128 bytes, 5 in, quarter") << 128 << 5 << 25 << 5 << 85;
        QTest::addRow("64 bytes, 9 in, q95") << 64 << 9 << 40 << 6 << 95;
        QTest::addRow("200 bytes, 2 in, q60") << 200 << 2 << 65 << 7 << 60;
        QTest::addRow("17 bytes, 12 in, q75") << 17 << 12 << 35 << 8 << 75;
        QTest::addRow("400 bytes, 0 in, q90") << 400 << 0 << 70 << 9 << 90;
        // A broader, randomized sweep, slow enough to keep out of the default
        // run: MCU_STUDIO_LONG_TESTS=1.
        if (qEnvironmentVariableIsSet("MCU_STUDIO_LONG_TESTS")) {
            const int lengths[] = {1, 2, 5, 16, 33, 64, 100, 256, 511};
            const int qualities[] = {50, 70, 85, 92, 95};
            quint32 r = 1234;
            const auto next = [&r](int n) {
                r = r * 1664525u + 1013904223u;
                return int((r >> 8) % unsigned(n));
            };
            for (int i = 0; i < 40; ++i) {
                const int jb = lengths[next(9)], into = next(16), where = 10 + next(81), q = qualities[next(5)];
                QTest::addRow("sweep %d: %d bytes, %d in, %d%%, q%d", i, jb, into, where, q)
                    << jb << into << where << 100 + i << q;
            }
        }
    }
    void resyncSkipsJunkInsideAnMcu()
    {
        QFETCH(int, junkBytes);
        QFETCH(int, intoMcu);
        QFETCH(int, where);
        QFETCH(int, seed);
        QFETCH(int, quality);
        Spec s;
        s.width = 640;
        s.height = 480;
        s.seed = seed;
        s.quality = quality;
        const QByteArray clean = testutil::make(s);
        const bitstream::Map m = bitstream::map(clean);
        const int victim = int(qint64(m.mcus.size()) * where / 100);
        // Junk without FF, so nothing in it reads as a marker.
        QByteArray junk;
        for (int i = 0; i < junkBytes; ++i)
            junk.append(char((i * 73 + seed * 31 + 5) % 251));
        const qsizetype byte = qsizetype(m.mcus[victim].bitPos >> 3) + 1 + intoMcu;
        QByteArray broken = clean;
        broken.insert(byte, junk);
        QVERIFY(bitstream::map(broken).firstAnomaly >= 0 || coefBytes(broken) != coefBytes(clean));

        const qint64 start = m.mcus[victim].bitPos; // before the junk: same in both files
        const QVector<bitstream::Candidate> found = bitstream::searchResync(broken, start, 64, 600);
        QVERIFY(!found.isEmpty());

        // The winner cuts the junk: everything after the damage comes back
        // exactly, and at most the MCUs that held it keep a trace (cutting a
        // byte next to the junk instead of the junk itself looks the same to
        // any measure, and leaves one byte of garbage in one MCU).
        // Rank at which a candidate first gives the picture back: everything
        // after the damage exactly, and at most the MCUs that held it keeping
        // a trace (cutting a byte next to the junk instead of the junk itself
        // can look the same to every measure, and leaves one byte of garbage
        // in one MCU). The UI previews the candidates in order, so the right
        // one has to be near the top, not necessarily first.
        auto b = jr::Coefs::load(clean);
        const int per = b->info().blocksPerMcu * 64;
        int goodRank = -1;
        for (int rank = 0; rank < found.size() && rank < 10 && goodRank < 0; ++rank) {
            const auto fixed = bitstream::deleteBits(broken, found[rank].bitPos, found[rank].deltaBits);
            if (!fixed)
                continue;
            auto a = jr::Coefs::load(*fixed);
            int differing = 0;
            bool local = true;
            for (int i = 0; i < b->info().mcuCount() && a; ++i) {
                if (!std::equal(a->mcu(i), a->mcu(i) + per, b->mcu(i))) {
                    ++differing;
                    local = local && i >= victim && i <= victim + 2;
                }
            }
            if (a && differing <= 2 && local)
                goodRank = rank;
        }
        if (qEnvironmentVariableIsSet("MCU_STUDIO_LONG_TESTS"))
            qInfo("first good fix at rank %d", goodRank);
        QVERIFY(goodRank >= 0);
        QVERIFY2(goodRank < 5, qPrintable(QStringLiteral("first good fix at rank %1").arg(goodRank)));
    }

    // --- donor headers --------------------------------------------------------

    void spliceRecoversHeaderlessData()
    {
        Spec s;
        s.width = 1024;
        s.height = 768;
        const QByteArray clean = testutil::make(s);
        Spec d = s;
        d.seed = 5; // another shot, same camera settings
        const QByteArray donorBytes = testutil::make(d);
        // Destroy the first 150 KiB, the way STOP/Djvu does.
        QByteArray broken = clean;
        const qsizetype lost = qMin<qsizetype>(0x25800, clean.size() / 3);
        for (qsizetype i = 0; i < lost; ++i)
            broken[i] = char((i * 131 + 7) & 0xFF);
        const donor::Layout layout = donor::scan(donorBytes);
        QString error;
        const auto spliced = donor::splice(donorBytes, layout, broken, lost, &error);
        QVERIFY2(spliced, qPrintable(error));
        const bitstream::Map m = bitstream::map(spliced->bytes);
        QVERIFY(m.isValid());
        // After the first few MCUs (the splice lands mid-MCU) the data decodes.
        int clean_ = 0;
        for (const bitstream::McuRecord &r : m.mcus)
            clean_ += r.anomalies == 0 ? 1 : 0;
        QVERIFY2(clean_ > m.mcus.size() / 3, "spliced data does not decode under the donor's tables");
    }

    void spliceKeepsOwnTables()
    {
        Spec s;
        s.quality = 70;
        const QByteArray clean = testutil::make(s);
        Spec d = s;
        d.quality = 95; // a donor with the wrong tables
        const QByteArray donorBytes = testutil::make(d);
        // Damage the frame header so the file will not open on its own.
        QByteArray broken = clean;
        const jpegfile::Structure st = jpegfile::walk(clean);
        broken[st.frameOffset + 1] = char(0xC8); // JPG reserved: not a frame
        QVERIFY(!jr::probe(broken));
        const donor::Layout bl = donor::scan(broken);
        donor::SpliceOptions keep;
        keep.keepOwnTables = true;
        QString error;
        const auto spliced = donor::splice(donorBytes, donor::scan(donorBytes), broken,
                                           st.scans.first().dataStart, &error, keep);
        QVERIFY(spliced);
        QVERIFY(spliced->keptOwn.contains(QStringLiteral("DQT 0")));
        // With the file's own tables the picture decodes to exactly what it was.
        QCOMPARE(coefBytes(spliced->bytes), coefBytes(clean));
        Q_UNUSED(bl);
    }

    void spliceRenumbersRestarts()
    {
        Spec s;
        s.restartInterval = 4;
        const QByteArray clean = testutil::make(s);
        const qsizetype start = scanStart(clean);
        // Splice right after an RST3.
        qsizetype at = -1;
        for (qsizetype i = start; i + 1 < clean.size(); ++i) {
            if (quint8(clean[i]) == 0xFF && quint8(clean[i + 1]) == 0xD3 && i > start + 2000) {
                at = i + 2;
                break;
            }
        }
        QVERIFY(at > 0);
        const auto spliced = donor::splice(clean, donor::scan(clean), clean, at, nullptr);
        QVERIFY(spliced);
        QVERIFY(spliced->renumberedRestarts > 0);
        const QByteArray data = spliced->bytes.mid(spliced->headerSize);
        const qsizetype first = data.indexOf(QByteArray("\xFF\xD0", 2));
        for (int n = 1; n < 8; ++n) {
            const qsizetype other = data.indexOf(QByteArray(1, char(0xFF)) + char(0xD0 + n));
            QVERIFY(other < 0 || other > first);
        }
        QVERIFY(bitstream::map(spliced->bytes).mcus.value(0).anomalies == 0);
    }

    void donorRankingPrefersMatchingTables()
    {
        QTemporaryDir dir;
        Spec s;
        s.width = 800;
        s.height = 600;
        s.quality = 80;
        QByteArray damaged = testutil::make(s); // standard Huffman tables
        const qsizetype dataStart = scanStart(damaged);
        // Its header is gone: nothing of its own to keep.
        damaged.replace(0, dataStart, QByteArray(dataStart, '\0'));
        Spec same = s;
        same.seed = 3; // another shot from the same camera
        Spec other = s;
        other.seed = 4;
        other.optimize = true; // its own Huffman tables: wrong for this data
        const QString pSame = writeTemp(dir, QStringLiteral("same.jpg"), testutil::make(same));
        const QString pOther = writeTemp(dir, QStringLiteral("other.jpg"), testutil::make(other));
        const auto ranked = donorsearch::rank(damaged, dataStart, {pOther, pSame});
        QCOMPARE(ranked.size(), 2);
        QCOMPARE(ranked.first().path, pSame);
        QVERIFY(ranked.first().score > ranked.last().score);
    }

    void widthDetectionFindsTheFrame()
    {
        Spec s;
        s.width = 640;
        s.height = 480;
        const QByteArray clean = testutil::make(s);
        Spec wrong = s;
        wrong.width = 720; // the donor came from a different resolution
        wrong.height = 480;
        const QByteArray donorBytes = testutil::make(wrong);
        const auto found = donorsearch::detectWidth(donorBytes, clean, scanStart(clean), {}, 512, 800);
        QVERIFY(!found.isEmpty());
        QCOMPARE(found.first().width, 640);
    }

    // --- analysis --------------------------------------------------------------

    void autoAlignFindsTheShift()
    {
        Spec s;
        s.width = 640;
        s.height = 480;
        auto c = jr::Coefs::load(testutil::make(s));
        const jr::Info info = c->info();
        const int start = 10 * info.mcusX + 7;
        QVERIFY(c->apply({jr::Op::insertMcus(9, jr::Scope::runFrom(10, 7))}));
        const auto found = analysis::autoAlign(info, c->render(true), start, 2 * info.mcusX);
        QVERIFY(!found.isEmpty());
        QCOMPARE(found.first().shift, -9);
    }

    void autoDcFindsTheOffset()
    {
        Spec s;
        s.width = 640;
        s.height = 480;
        auto c = jr::Coefs::load(testutil::make(s));
        const jr::Info info = c->info();
        const int start = 12 * info.mcusX + 3;
        QVERIFY(c->apply({jr::Op::cdelta(0, 25, jr::Scope::runFrom(12, 3)),
                          jr::Op::cdelta(1, -8, jr::Scope::runFrom(12, 3))}));
        QByteArray mask(info.mcuCount(), '\0');
        for (int m = start; m < mask.size(); ++m)
            mask[m] = 1;
        const analysis::DcEstimate est = analysis::autoDc(*c, mask);
        QVERIFY(est.valid);
        QVERIFY2(std::abs(est.cdelta[0] + 25) <= 2, qPrintable(QString::number(est.cdelta[0])));
        QVERIFY2(std::abs(est.cdelta[1] - 8) <= 2, qPrintable(QString::number(est.cdelta[1])));
        QVERIFY(est.confidence > 0.5);
    }

    void damageMapFindsTheEdge()
    {
        Spec s;
        s.width = 640;
        s.height = 480;
        auto c = jr::Coefs::load(testutil::make(s));
        const jr::Info info = c->info();
        QVERIFY(c->apply({jr::Op::cdelta(0, 60, jr::Scope::runFrom(15, 0))}));
        const analysis::DamageMap d = analysis::damage(info, c->render(true));
        QVERIFY(d.firstDamaged >= 15 * info.mcusX - 1);
        QVERIFY(d.firstDamaged <= 15 * info.mcusX + 1);
    }

    void triageVerdicts()
    {
        const QByteArray clean = testutil::make(Spec{});
        QCOMPARE(analysis::triage(clean).verdict, analysis::Triage::Verdict::Healthy);
        QByteArray truncated = clean.left(clean.size() / 2);
        QCOMPARE(analysis::triage(truncated).verdict, analysis::Triage::Verdict::Truncated);
        QByteArray noHeader = clean;
        noHeader.replace(0, 400, QByteArray(400, '\0'));
        QCOMPARE(analysis::triage(noHeader).verdict, analysis::Triage::Verdict::HeaderDamaged);
    }

    // --- the document -------------------------------------------------------

    void autoColorKeepsGeometry()
    {
        QTemporaryDir dir;
        Spec s;
        s.h0 = s.v0 = 1; // 4:4:4: the old auto color re-encoded as 4:2:0
        const QString path = writeTemp(dir, QStringLiteral("a.jpg"), testutil::make(s));
        ImageDocument doc;
        QString error;
        QVERIFY(doc.load(path, &error));
        const jr::Info before = doc.info();
        QVERIFY2(doc.addAutoColor(QStringLiteral("auto"), &error), qPrintable(error));
        QCOMPARE(doc.info().mcusX, before.mcusX);
        QCOMPARE(doc.info().blocksPerMcu, before.blocksPerMcu);
        QVERIFY(doc.addOps({jr::Op::insertMcus(2, jr::Scope::runFrom(1, 1))}, QStringLiteral("i"), &error));
        const auto out = doc.exportBytes(&error);
        QVERIFY(out);
        QCOMPARE(jr::probe(*out)->samplingName(), QStringLiteral("4:4:4"));
    }

    void byteEditStepsComeFirstAndReplay()
    {
        QTemporaryDir dir;
        Spec s;
        s.width = 640;
        s.height = 480;
        const QByteArray clean = testutil::make(s);
        const bitstream::Map m = bitstream::map(clean);
        const qint64 at = m.mcus[m.mcus.size() / 2].bitPos;
        const auto broken = bitstream::insertBits(clean, at, 11, 1);
        const QString path = writeTemp(dir, QStringLiteral("b.jpg"), *broken);

        ImageDocument doc;
        QString error;
        QVERIFY(doc.load(path, &error));
        QVERIFY(doc.addOps({jr::Op::cdelta(0, 1, jr::Scope::wholeImage())}, QStringLiteral("dc"), &error));
        ByteEdit e;
        e.kind = ByteEdit::Kind::DeleteBits;
        e.offset = at >> 3;
        e.bit = int(at & 7);
        e.count = 11;
        QVERIFY2(doc.addByteEdits({e}, QStringLiteral("resync"), &error), qPrintable(error));
        QCOMPARE(doc.steps().first().kind, RepairStep::Kind::Bytes);

        // Project round trip, history included.
        project::Project p;
        p.sourcePath = path;
        p.sourceSha256 = doc.sourceSha256();
        p.steps = doc.steps();
        p.history = doc.history();
        p.historyIndex = doc.historyIndex();
        const QString projectPath = path + QStringLiteral(".mcup");
        QVERIFY(project::write(projectPath, p, &error));
        const auto back = project::read(projectPath, &error);
        QVERIFY2(back, qPrintable(error));
        QCOMPARE(back->steps.size(), 2);
        QCOMPARE(back->steps.first().edits.first(), e);
        QCOMPARE(back->history.size(), doc.history().size());
        QCOMPARE(back->sourceSha256, doc.sourceSha256());

        ImageDocument again;
        QVERIFY(again.load(path, &error));
        QVERIFY(again.adoptSteps(back->steps, &error, back->history, back->historyIndex));
        QVERIFY(again.canUndo());
        const auto fixed = again.exportBytes(&error);
        auto expect = jr::Coefs::load(clean);
        expect->apply({jr::Op::cdelta(0, 1, jr::Scope::wholeImage())});
        QCOMPARE(coefBytes(*fixed),
                 QByteArray(reinterpret_cast<const char *>(expect->raw()->data),
                            qsizetype(expect->raw()->unit_count * 128)));
    }

    void undoRedo()
    {
        QTemporaryDir dir;
        const QString path = writeTemp(dir, QStringLiteral("u.jpg"), testutil::make(Spec{}));
        ImageDocument doc;
        QString error;
        QVERIFY(doc.load(path, &error));
        const QByteArray rgb0 = doc.rgb().data;
        QVERIFY(doc.addOps({jr::Op::cdelta(0, 10, jr::Scope::wholeImage())}, QStringLiteral("a"), &error));
        const QByteArray rgb1 = doc.rgb().data;
        QVERIFY(rgb1 != rgb0);
        QVERIFY(doc.undo());
        QCOMPARE(doc.rgb().data, rgb0);
        QVERIFY(doc.redo());
        QCOMPARE(doc.rgb().data, rgb1);
    }

    void provenanceTracksMovesAndPastes()
    {
        QTemporaryDir dir;
        const QString path = writeTemp(dir, QStringLiteral("p.jpg"), testutil::make(Spec{}));
        ImageDocument doc;
        QString error;
        QVERIFY(doc.load(path, &error));
        const jr::Info info = doc.info();
        QVERIFY(doc.addOps({jr::Op::insertMcus(2, jr::Scope::runFrom(3, 0))}, QStringLiteral("i"), &error));
        const auto clip = doc.coefs()->readMcus(0, 0, 1);
        QVERIFY(doc.addOps({jr::Op::paste(1, 1, *clip)}, QStringLiteral("p"), &error));
        const QByteArray flags = doc.provenance();
        QVERIFY(quint8(flags[0]) == 0);
        QVERIFY(quint8(flags[info.mcusX + 1]) & JR_TRACE_PASTED);
        QVERIFY(quint8(flags[3 * info.mcusX + 5]) & JR_TRACE_MOVED);
        const QJsonObject r = report::build(doc, path, *doc.exportBytes(&error));
        QCOMPARE(r.value(QStringLiteral("mcus")).toObject().value(QStringLiteral("pastedOrSynthesized")).toInt(), 1);
    }

    // --- Exif, carving ---------------------------------------------------------

    void exifThumbnailRefresh()
    {
        Spec t;
        t.width = 32;
        t.height = 24;
        const QByteArray thumb = testutil::make(t);
        const QByteArray file = testutil::withExifThumbnail(testutil::make(Spec{}), thumb);
        QCOMPARE(*exif::thumbnail(file), thumb);
        t.seed = 7;
        const QByteArray newThumb = testutil::make(t);
        const QByteArray out = exif::withThumbnail(file, newThumb);
        QCOMPARE(*exif::thumbnail(out), newThumb);
        QVERIFY(jr::probe(out)); // still a readable JPEG
        QCOMPARE(exif::orientation(out), 1);
    }

    void carveFindsEveryJpeg()
    {
        Spec a;
        a.width = 200;
        a.height = 100;
        Spec b;
        b.width = 120;
        b.height = 90;
        b.seed = 4;
        const QByteArray ja = testutil::make(a), jb = testutil::make(b);
        const QByteArray blob = QByteArray(5000, '\x11') + ja + QByteArray(333, '\x22') + jb + QByteArray(100, '\0');
        const QVector<jpegfile::Embedded> found = jpegfile::carve(blob, 10, 100);
        QCOMPARE(found.size(), 2);
        QCOMPARE(blob.mid(found[0].offset, found[0].length), ja.size() > jb.size() ? ja : jb);
    }
};

QTEST_GUILESS_MAIN(CoreTest)
#include "tst_core.moc"
