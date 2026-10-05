// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QTimer>

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

    // The icon is embedded (see qt_add_resources in CMakeLists) so it is there
    // even when running uninstalled. The desktop file name is what ties the
    // window to packaging/mcu-studio.desktop: it becomes the Wayland app_id and
    // the X11 WM_CLASS, which is how the shell finds the installed theme icon.
    // Without it Qt would derive them from the executable path, which only
    // happens to match when the binary is named exactly like the .desktop file.
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/mcu-studio.ico")));
    QGuiApplication::setDesktopFileName(QStringLiteral("mcu-studio"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QCoreApplication::translate("main",
                                    "Repair damaged JPEG images by editing their bitstream and "
                                    "DCT coefficients: realign the stream, correct DC drift per "
                                    "component, transplant headers, and fill lost MCUs."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QCoreApplication::translate("main", "image"),
                                 QCoreApplication::translate("main", "JPEG to open on start."));
    // For CI and documentation: open the image, wait for it to render, save
    // a picture of the window and quit.
    QCommandLineOption screenshot(QStringLiteral("screenshot"),
                                  QCoreApplication::translate("main", "Save a screenshot of the window to <png> and exit."),
                                  QStringLiteral("png"));
    parser.addOption(screenshot);
    parser.process(app);

    MainWindow window;
    window.show();

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty())
        window.openFile(args.first());

    if (parser.isSet(screenshot)) {
        const QString target = parser.value(screenshot);
        QTimer::singleShot(1500, &window, [&window, target] {
            const bool ok = window.grab().save(target);
            QCoreApplication::exit(ok ? 0 : 1);
        });
    }

    return app.exec();
}