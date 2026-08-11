// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include <QApplication>
#include <QCommandLineParser>

#include "MainWindow.h"
#include "PlatformStyle.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    // Follow the desktop's light/dark scheme. The image canvas keeps its own
    // dark backdrop either way -- see McuGraphicsView's constructor.
    applyPlatformStyle();

    QCoreApplication::setApplicationName(QStringLiteral("MCU Studio"));
    QCoreApplication::setApplicationVersion(QStringLiteral(MCU_STUDIO_VERSION));
    QCoreApplication::setOrganizationName(QStringLiteral("TheGameratorT"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QCoreApplication::translate("main",
                                    "Repair JPEG images by editing their DCT coefficients: "
                                    "shift color per component, and insert, delete or copy "
                                    "MCU blocks."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QCoreApplication::translate("main", "image"),
                                 QCoreApplication::translate("main", "JPEG to open on start."));
    parser.process(app);

    MainWindow window;
    window.show();

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty())
        window.openFile(args.first());

    return app.exec();
}