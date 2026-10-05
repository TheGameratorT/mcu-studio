// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// The entropy-coded stream, read at the level of bits.
//
// Everything else in this tool works on coefficients, which is to say after
// libjpeg has already decided what a damaged stream means. Most damage is not
// made of whole MCUs, though. A flipped bit or a few lost bytes throw the
// Huffman decoder off for a stretch, after which it usually falls back into
// step on its own -- with every later block shifted by however much was lost
// and every DC predictor off by whatever the garbage summed to. Shifting MCUs
// and offsetting DC only approximates that. Putting the bits back is exact.
//
// This module decodes the stream itself, recording where each MCU starts in
// the file and where decoding stopped making sense, and it edits the stream:
// deleting or inserting bits or bytes at a position, keeping the byte stuffing
// and restart markers well-formed. It also searches for the edit that puts a
// desynchronized stream back in step, by decoding from the damage point under
// each candidate and keeping the one that decodes cleanly the longest.
#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <optional>

namespace bitstream {

enum Anomaly : quint8 {
    BadCode = 1,        // bits that are no code in the Huffman table
    CoefOverflow = 2,   // a run of AC coefficients past the 64th
    MarkerInMcu = 4,    // a marker in the middle of an MCU's data
    RestartOrder = 8,   // a restart marker with the wrong number
    RestartMissing = 16, // no restart marker where the interval said one goes
    Truncated = 32,     // the data ran out
    ValueRange = 64,    // a coefficient magnitude no 8-bit encoder produces
};

struct McuRecord {
    qint64 bitPos = 0; // file byte offset * 8 + bit (0 = most significant)
    int bits = 0;      // how many bits of data it took
    quint8 anomalies = 0;
};

struct Map {
    QVector<McuRecord> mcus; // in scan order, for the first scan
    int mcusX = 0, mcusY = 0;
    int components = 0; // in the scan
    // Per MCU and component: the DC predictor going into the MCU, and the
    // DC value of the component's first block in it. Quantized units.
    QVector<int> predIn, dc;
    int restartInterval = 0;
    qsizetype scanStart = 0, scanEnd = 0;
    // The first MCU with an anomaly, -1 if the scan decoded cleanly.
    int firstAnomaly = -1;
    int anomalyCount = 0;
    // Why there is no map, when there is not.
    QString unsupported;

    bool isValid() const { return unsupported.isEmpty() && !mcus.isEmpty(); }
    // The MCU whose data contains file byte `offset`, or -1.
    int mcuAtByte(qsizetype offset) const;
};

// Maps the first scan of `jpeg`. Supports what cameras write: sequential
// Huffman-coded scans (SOF0/SOF1), interleaved or single-component. A
// progressive or arithmetic-coded file gets a Map that says so.
Map map(const QByteArray &jpeg);

// A list of names for an anomaly bit set, for the UI.
QString describe(quint8 anomalies);

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

// Deletes `count` bits of entropy-coded data starting at `bitPos` (byte*8 +
// bit, as McuRecord counts them). The bits are taken from the unstuffed
// stream, so a stuffed FF 00 counts as eight bits, not sixteen; the rest of
// the restart interval slides back, its tail is padded with 1-bits to a byte
// boundary as an encoder pads it, and the stuffing is rewritten. Fails if
// `bitPos` is not inside a scan's data.
std::optional<QByteArray> deleteBits(const QByteArray &jpeg, qint64 bitPos, int count,
                                     QString *error = nullptr);
// Inserts `count` bits of value `bit` (0 or 1) at `bitPos`.
std::optional<QByteArray> insertBits(const QByteArray &jpeg, qint64 bitPos, int count, int bit = 0,
                                     QString *error = nullptr);
// Flips one bit of the file. Done on the raw bytes, stuffing and all: this is
// for undoing a single flipped bit, which happens in the stored bytes.
std::optional<QByteArray> flipBit(const QByteArray &jpeg, qint64 bitPos, QString *error = nullptr);

// ---------------------------------------------------------------------------
// Searching
// ---------------------------------------------------------------------------

struct Candidate {
    int deltaBits = 0;  // > 0: delete this many bits; < 0: insert -deltaBits zero bits
    qint64 bitPos = 0;  // where the edit goes (byte*8 + bit), which need not be the MCU start
    int cleanMcus = 0;  // MCUs decoded after the edit before the next anomaly
    bool reachedEnd = false; // decoded to the end of the window or the next restart
    // The DC offset the edit leaves behind, measured against the MCU rows
    // above it, which it did not touch: garbage left in place shifts every
    // later DC predictor by the same amount, so the right edit is the one
    // whose picture carries on from the rows above with no offset. The
    // signed median difference, so that texture cancels out; in sample
    // levels, lower is better.
    double dcStep = 0;
    // How many more bits the first MCUs after the edit took than the window's
    // typical MCU: garbage left in place decodes long. Breaks ties between
    // edits whose DC continues equally well.
    int excessBits = 0;
    // Luma discontinuity, in sample levels, across the edges of the first
    // two MCUs after the edit where they meet MCUs the edit did not touch.
    // Garbage left inside them shows here first.
    double edgeCost = 0;
};

// Tries every edit of up to `maxBits` bits either way at `bitPos` (which
// should be the start of the MCU where damage begins), then every longer
// deletion up to `maxBytes` bytes, decoding up to `window` MCUs after each.
// A long deletion that wins usually removed the rest of a damaged MCU along
// with the garbage, so the stream is then whole MCUs short and wants an MCU
// insert to line it back up. Returns the candidates ranked best first (most
// clean MCUs, then the smallest edit); the do-nothing candidate is included.
QVector<Candidate> searchResync(const QByteArray &jpeg, qint64 bitPos, int maxBits = 64,
                                int maxBytes = 2048, int window = 400);

} // namespace bitstream
