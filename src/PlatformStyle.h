// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#pragma once

// Pick a widget style that honours the system light/dark colour scheme on every
// supported platform.
//
// Qt's colorScheme() already follows the OS appearance, but on Windows 10 the
// default (legacy) Windows style draws its chrome from UxTheme, which stays
// light regardless of that setting -- so a dark-mode desktop still renders a
// light app. Windows 11 uses the windows11 style, which does track the scheme,
// so we leave it alone (and a Windows 10 box shouldn't be made to wear the
// Windows 11 look). For Windows 10 and earlier we switch to Fusion, which is
// painted entirely from the palette and therefore goes dark with the system.
// Other platforms keep their native style untouched.
//
// Call once, right after the QApplication is constructed and before any windows
// are shown.

#include <QApplication>
#include <QPalette>
#include <QString>

#ifdef Q_OS_WIN
#include <QOperatingSystemVersion>
#endif

inline void applyPlatformStyle()
{
#ifdef Q_OS_WIN
    if (QOperatingSystemVersion::current() < QOperatingSystemVersion::Windows11)
        QApplication::setStyle(QStringLiteral("Fusion"));
#endif
}

// Amber for "this may not be trustworthy". The shade that reads on a dark
// background is far too pale on a light one, so pick per scheme -- each sits
// around 6:1 against its own window colour. Re-ask after a palette change: the
// answer is a fixed hex chosen for the scheme that was in force.
inline QString warningTextStyle()
{
    const QPalette pal = QApplication::palette();
    const bool dark = pal.color(QPalette::Window).lightness()
                      < pal.color(QPalette::WindowText).lightness();
    return QStringLiteral("color: %1;")
        .arg(dark ? QStringLiteral("#e0a030") : QStringLiteral("#8a5300"));
}
