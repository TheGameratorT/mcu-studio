// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "MainWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsScene>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QInputDialog>
#include <QJsonDocument>
#include <QProgressDialog>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTime>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <cmath>

#include "AnalysisDock.h"
#include "AutoColor.h"
#include "BatchDialog.h"
#include "DonorHeader.h"
#include "DonorHeaderDialog.h"
#include "EmbeddedImagesDialog.h"
#include "Exif.h"
#include "JpegStructure.h"
#include "Report.h"
#include "McuGraphicsView.h"
#include "PlatformStyle.h"
#include "ReferenceColorDialog.h"
#include "ReferenceFill.h"
#include "ReferenceFillDialog.h"

namespace {

// Long enough that dragging a slider does not queue a render per pixel, short
// enough that letting go feels immediate.
constexpr int kPreviewDebounceMs = 90;

// Above this the mean over the selection costs more than it tells you, and it
// would be recomputed on every mouse move during a drag.
constexpr qint64 kMaxMeasuredMcus = 60000;

// A block whose samples are this heavily clipped makes a color match unsafe.
constexpr double kClippedSampleWarning = 0.01;

// Beyond this the reference and target probably are not the same content.
constexpr double kLargePixelDelta = 60.0;

using colormath::formatTriple;

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    buildActions();
    buildUi();
    setAcceptDrops(true);
    updateWindowTitle();
    refreshStepList();
    updateActionStates();
    updateImageInfo();
    updateSelectionInfo();
    updateClipboardInfo();
    log(tr("Ready. Open a JPEG to begin."));
}

MainWindow::~MainWindow()
{
    // The preview task holds only copies, but the watcher must not deliver
    // into a half-destroyed window.
    if (m_previewWatcher)
        m_previewWatcher->disconnect(this);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void MainWindow::buildActions()
{
    m_openAction = new QAction(tr("&Open…"), this);
    m_openAction->setShortcut(QKeySequence::Open);
    connect(m_openAction, &QAction::triggered, this, &MainWindow::onOpen);

    m_openDonorAction = new QAction(tr("Open with &Donor Header…"), this);
    m_openDonorAction->setToolTip(tr("Open a JPEG whose own header is damaged, borrowing one "
                                     "from another shot on the same card."));
    connect(m_openDonorAction, &QAction::triggered, this, &MainWindow::onOpenWithDonor);

    // There is no Save: the repair is written to its project file as it is
    // made. What is left to ask for is the repaired picture itself, which is a
    // product of the session rather than the session, and so is exported.
    m_exportAction = new QAction(tr("&Export JPEG"), this);
    m_exportAction->setShortcut(QKeySequence::Save);
    m_exportAction->setToolTip(tr("Write the repaired picture out. Asks where the first time, "
                                  "then goes on writing to the same file."));
    connect(m_exportAction, &QAction::triggered, this, &MainWindow::onExport);

    m_exportAsAction = new QAction(tr("Export JPEG &As…"), this);
    m_exportAsAction->setShortcut(QKeySequence::SaveAs);
    connect(m_exportAsAction, &QAction::triggered, this, &MainWindow::onExportAs);

    m_undoAction = new QAction(tr("&Undo"), this);
    m_undoAction->setShortcut(QKeySequence::Undo);
    connect(m_undoAction, &QAction::triggered, this, &MainWindow::onUndo);

    m_redoAction = new QAction(tr("&Redo"), this);
    m_redoAction->setShortcut(QKeySequence::Redo);
    connect(m_redoAction, &QAction::triggered, this, &MainWindow::onRedo);

    m_resetAction = new QAction(tr("Revert to &Original"), this);
    connect(m_resetAction, &QAction::triggered, this, &MainWindow::onResetToOriginal);

    m_zoomFitAction = new QAction(tr("&Fit to Window"), this);
    m_zoomFitAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    m_zoomActualAction = new QAction(tr("&Actual Size"), this);
    m_zoomActualAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_1));
    m_zoomInAction = new QAction(tr("Zoom &In"), this);
    m_zoomInAction->setShortcut(QKeySequence::ZoomIn);
    m_zoomOutAction = new QAction(tr("Zoom &Out"), this);
    m_zoomOutAction->setShortcut(QKeySequence::ZoomOut);

    m_gridAction = new QAction(tr("Show MCU &Grid"), this);
    m_gridAction->setCheckable(true);
    m_gridAction->setChecked(true);
    m_gridAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));

    m_selectionOverlayAction = new QAction(tr("Show &Selection Overlay"), this);
    m_selectionOverlayAction->setCheckable(true);
    m_selectionOverlayAction->setChecked(true);

    m_selectAllAction = new QAction(tr("Select &All MCUs"), this);
    m_selectAllAction->setShortcut(QKeySequence::SelectAll);
    m_clearSelectionAction = new QAction(tr("&Clear Selection"), this);
    m_clearSelectionAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));

    m_copySelectionAction = new QAction(tr("&Copy MCUs"), this);
    m_copySelectionAction->setShortcut(QKeySequence::Copy);
    m_copySelectionAction->setToolTip(tr("Lift the selected MCUs onto the clipboard."));
    connect(m_copySelectionAction, &QAction::triggered, this, &MainWindow::onCopySelection);

    m_pasteOverAction = new QAction(tr("&Paste Over"), this);
    m_pasteOverAction->setShortcut(QKeySequence::Paste);
    m_pasteOverAction->setToolTip(
        tr("Write the clipboard over the MCUs from the selection onward."));
    connect(m_pasteOverAction, &QAction::triggered, this, &MainWindow::onPasteOver);

    m_pasteInsertAction = new QAction(tr("Paste &Inserting"), this);
    m_pasteInsertAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V));
    m_pasteInsertAction->setToolTip(
        tr("Push the rest of the image along to make room, then write the clipboard in."));
    connect(m_pasteInsertAction, &QAction::triggered, this, &MainWindow::onPasteInsert);

    m_fillReferenceAction = new QAction(tr("&Fill from Reference Picture…"), this);
    m_fillReferenceAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    m_fillReferenceAction->setToolTip(
        tr("Replace the MCUs in scope with content from another copy of the photograph, in "
           "any format. Compresses only those MCUs."));
    connect(m_fillReferenceAction, &QAction::triggered, this, &MainWindow::onFillFromReference);

    m_compareAction = new QAction(tr("Show &Original"), this);
    m_compareAction->setCheckable(true);
    m_compareAction->setShortcut(QKeySequence(Qt::Key_Backslash));
    m_compareAction->setToolTip(tr("Show the file as it was opened, before any repair step. "
                                   "Toggle it to compare."));
    connect(m_compareAction, &QAction::toggled, this, [this](bool on) {
        if (!m_doc.isOpen())
            return;
        m_grid->setPixmap(QPixmap::fromImage((on ? m_doc.originalRgb() : m_doc.rgb()).toImage()));
        m_showingPreview = false;
        if (!on)
            schedulePreview();
    });

    m_nextDamageAction = new QAction(tr("Select &Next Damaged MCU"), this);
    m_nextDamageAction->setShortcut(QKeySequence(Qt::Key_F3));
    m_nextDamageAction->setToolTip(tr("Moves the selection to the next MCU, in scan order, that "
                                      "either failed to decode or disagrees with its neighbors far "
                                      "more than the picture's own detail explains."));
    connect(m_nextDamageAction, &QAction::triggered, this, &MainWindow::onNextDamage);

    QSettings settings;
    m_reportAction = new QAction(tr("Write a Recovery &Report With Each Export"), this);
    m_reportAction->setCheckable(true);
    m_reportAction->setChecked(settings.value(QStringLiteral("export/report"), false).toBool());
    m_reportAction->setToolTip(tr("Writes <export>.report.json beside the picture: hashes of input "
                                  "and output, every step, and how many MCUs were moved, adjusted, "
                                  "synthesized or never recovered."));
    connect(m_reportAction, &QAction::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("export/report"), on);
    });
    m_thumbnailAction = new QAction(tr("Refresh the Exif &Thumbnail on Export"), this);
    m_thumbnailAction->setCheckable(true);
    m_thumbnailAction->setChecked(settings.value(QStringLiteral("export/thumbnail"), true).toBool());
    m_thumbnailAction->setToolTip(tr("The embedded thumbnail shows the picture as it was before the "
                                     "repair. Replace it with one of the repaired picture."));
    connect(m_thumbnailAction, &QAction::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("export/thumbnail"), on);
    });
    m_trailerAction = new QAction(tr("Keep Data After the Image on Export"), this);
    m_trailerAction->setCheckable(true);
    m_trailerAction->setChecked(settings.value(QStringLiteral("export/trailer"), true).toBool());
    m_trailerAction->setToolTip(tr("Carry an MPF preview or a motion photo's video along with the "
                                   "repaired picture. A ransomware footer is never carried."));
    connect(m_trailerAction, &QAction::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("export/trailer"), on);
    });
}

void MainWindow::buildUi()
{
    m_scene = new QGraphicsScene(this);
    m_grid = new McuGridItem;
    m_scene->addItem(m_grid);

    m_view = new McuGraphicsView(this);
    m_view->setScene(m_scene);
    setCentralWidget(m_view);

    connect(m_grid, &McuGridItem::selectionChanged, this, &MainWindow::onSelectionChanged);
    connect(m_grid, &McuGridItem::blockHovered, this, &MainWindow::onBlockHovered);
    connect(m_grid, &McuGridItem::blockPicked, this, &MainWindow::onBlockPicked);

    connect(m_zoomFitAction, &QAction::triggered, m_view, &McuGraphicsView::zoomToFit);
    connect(m_zoomActualAction, &QAction::triggered, m_view, &McuGraphicsView::zoomToActualSize);
    connect(m_zoomInAction, &QAction::triggered, this, [this] { m_view->zoomBy(1.25); });
    connect(m_zoomOutAction, &QAction::triggered, this, [this] { m_view->zoomBy(1.0 / 1.25); });
    connect(m_gridAction, &QAction::toggled, m_grid, &McuGridItem::setGridVisible);
    connect(m_selectionOverlayAction, &QAction::toggled, m_grid, &McuGridItem::setSelectionVisible);
    connect(m_selectAllAction, &QAction::triggered, m_grid, &McuGridItem::selectAll);
    connect(m_clearSelectionAction, &QAction::triggered, m_grid, &McuGridItem::clearSelection);

    // --- menus -------------------------------------------------------------
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(m_openAction);
    fileMenu->addAction(m_openDonorAction);
    fileMenu->addSeparator();
    fileMenu->addAction(m_exportAction);
    fileMenu->addAction(m_exportAsAction);
    QAction *writeReport = fileMenu->addAction(tr("Write Recovery Report…"));
    connect(writeReport, &QAction::triggered, this, &MainWindow::onWriteReport);
    QMenu *exportOptions = fileMenu->addMenu(tr("Export &Options"));
    exportOptions->addAction(m_reportAction);
    exportOptions->addAction(m_thumbnailAction);
    exportOptions->addAction(m_trailerAction);
    fileMenu->addSeparator();
    QAction *quit = fileMenu->addAction(tr("&Quit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(m_undoAction);
    editMenu->addAction(m_redoAction);
    editMenu->addSeparator();
    editMenu->addAction(m_resetAction);
    editMenu->addSeparator();
    editMenu->addAction(m_copySelectionAction);
    editMenu->addAction(m_pasteOverAction);
    editMenu->addAction(m_pasteInsertAction);
    editMenu->addSeparator();
    editMenu->addAction(m_fillReferenceAction);
    editMenu->addSeparator();
    editMenu->addAction(m_selectAllAction);
    editMenu->addAction(m_clearSelectionAction);

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(m_zoomInAction);
    viewMenu->addAction(m_zoomOutAction);
    viewMenu->addAction(m_zoomFitAction);
    viewMenu->addAction(m_zoomActualAction);
    viewMenu->addSeparator();
    viewMenu->addAction(m_gridAction);
    viewMenu->addAction(m_selectionOverlayAction);
    viewMenu->addAction(m_compareAction);
    viewMenu->addSeparator();
    m_overlayGroup = new QActionGroup(this);
    const struct {
        const char *text;
        Overlay overlay;
        QKeySequence key;
    } overlays[] = {
        {QT_TR_NOOP("No Overlay"), Overlay::None, QKeySequence()},
        {QT_TR_NOOP("Overlay &Damage Map"), Overlay::Damage, QKeySequence(Qt::CTRL | Qt::Key_D)},
        {QT_TR_NOOP("Overlay &Provenance"), Overlay::Provenance, QKeySequence(Qt::CTRL | Qt::Key_P)},
    };
    for (const auto &o : overlays) {
        QAction *a = viewMenu->addAction(tr(o.text));
        a->setCheckable(true);
        a->setShortcut(o.key);
        a->setChecked(o.overlay == Overlay::None);
        m_overlayGroup->addAction(a);
        const Overlay which = o.overlay;
        connect(a, &QAction::triggered, this, [this, which] {
            m_overlay = which;
            refreshOverlay();
        });
    }
    viewMenu->actions().last()->setToolTip(
        tr("Colors each MCU by what the recipe did to it: blue moved, yellow DC-adjusted, magenta "
           "pasted or synthesized, red no recovered data."));

    QMenu *toolsMenu = menuBar()->addMenu(tr("&Tools"));
    toolsMenu->addAction(m_nextDamageAction);
    QAction *align = toolsMenu->addAction(tr("&Align Stream at Selection…"));
    align->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_L));
    connect(align, &QAction::triggered, this, &MainWindow::onAutoAlign);
    QAction *autoDc = toolsMenu->addAction(tr("&Estimate DC Offset for Scope"));
    autoDc->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_E));
    connect(autoDc, &QAction::triggered, this, &MainWindow::onAutoDc);
    toolsMenu->addSeparator();
    const auto shiftAction = [this, toolsMenu](const QString &text, const QKeySequence &key,
                                               int mcus, int units) {
        QAction *a = toolsMenu->addAction(text);
        a->setShortcut(key);
        connect(a, &QAction::triggered, this, [this, mcus, units] { nudgeShift(mcus, units); });
    };
    shiftAction(tr("Preview Inserting One MCU"), QKeySequence(Qt::Key_BracketLeft), 1, 0);
    shiftAction(tr("Preview Deleting One MCU"), QKeySequence(Qt::Key_BracketRight), -1, 0);
    shiftAction(tr("Preview Inserting One Block"), QKeySequence(Qt::Key_BraceLeft), 0, 1);
    shiftAction(tr("Preview Deleting One Block"), QKeySequence(Qt::Key_BraceRight), 0, -1);
    QAction *commitShiftAction = toolsMenu->addAction(tr("Commit Previewed Shift"));
    commitShiftAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    connect(commitShiftAction, &QAction::triggered, this, &MainWindow::commitShift);
    QAction *cancelShiftAction = toolsMenu->addAction(tr("Cancel Previewed Shift"));
    cancelShiftAction->setShortcut(QKeySequence(Qt::Key_Escape));
    connect(cancelShiftAction, &QAction::triggered, this, &MainWindow::cancelShift);
    toolsMenu->addSeparator();
    QAction *embedded = toolsMenu->addAction(tr("&Pictures Inside This File…"));
    connect(embedded, &QAction::triggered, this, &MainWindow::onEmbeddedImages);
    QAction *batch = toolsMenu->addAction(tr("&Triage a Folder…"));
    connect(batch, &QAction::triggered, this, &MainWindow::onBatchTriage);
    QAction *carve = toolsMenu->addAction(tr("&Carve JPEGs out of a File…"));
    carve->setToolTip(tr("Finds every complete JPEG inside any file: a disk image, a camera RAW "
                         "file (whose full-size preview is a JPEG), a backup archive."));
    connect(carve, &QAction::triggered, this, &MainWindow::onCarve);

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    QAction *about = helpMenu->addAction(tr("&About MCU Studio"));
    connect(about, &QAction::triggered, this, &MainWindow::onAbout);
    // LGPL notice for the linked Qt libraries.
    QAction *aboutQt = helpMenu->addAction(tr("About &Qt"));
    connect(aboutQt, &QAction::triggered, qApp, &QApplication::aboutQt);

    // --- toolbar -----------------------------------------------------------
    QToolBar *toolbar = addToolBar(tr("Main"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toolbar->addAction(m_openAction);
    toolbar->addAction(m_exportAction);
    toolbar->addSeparator();
    toolbar->addAction(m_undoAction);
    toolbar->addAction(m_redoAction);
    toolbar->addAction(m_resetAction);
    toolbar->addSeparator();
    toolbar->addAction(m_zoomFitAction);
    toolbar->addAction(m_zoomActualAction);
    toolbar->addAction(m_gridAction);

    // --- docks -------------------------------------------------------------
    QDockWidget *stepDock = new QDockWidget(tr("Repair steps"), this);
    stepDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    stepDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    stepDock->setWidget(buildStepPanel());
    addDockWidget(Qt::LeftDockWidgetArea, stepDock);
    QAction *stepAction = viewMenu->addAction(tr("Show Repair &Steps"));
    stepAction->setCheckable(true);
    stepAction->setChecked(true);
    connect(stepAction, &QAction::toggled, stepDock, &QWidget::setVisible);
    connect(stepDock, &QDockWidget::visibilityChanged, stepAction, &QAction::setChecked);

    QDockWidget *controlDock = new QDockWidget(tr("Repair"), this);
    controlDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    controlDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    QScrollArea *scroll = new QScrollArea(controlDock);
    scroll->setWidgetResizable(true);
    scroll->setWidget(buildControlPanel());
    scroll->setMinimumWidth(340);
    controlDock->setWidget(scroll);
    addDockWidget(Qt::RightDockWidgetArea, controlDock);

    QDockWidget *logDock = new QDockWidget(tr("Log"), this);
    logDock->setAllowedAreas(Qt::BottomDockWidgetArea);
    m_log = new QPlainTextEdit(logDock);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    logDock->setWidget(m_log);
    addDockWidget(Qt::BottomDockWidgetArea, logDock);
    logDock->hide(); // available from the View menu, but out of the way by default

    QDockWidget *analysisDock = new QDockWidget(tr("Analysis"), this);
    analysisDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    m_analysisDock = new AnalysisDock(analysisDock);
    analysisDock->setWidget(m_analysisDock);
    addDockWidget(Qt::RightDockWidgetArea, analysisDock);
    tabifyDockWidget(controlDock, analysisDock);
    controlDock->raise();
    // The dock's own toggle action: a tabbed dock reports itself invisible
    // while another tab is in front, and a hand-wired action would hide it
    // for good on that signal.
    QAction *analysisAction = analysisDock->toggleViewAction();
    analysisAction->setText(tr("Show &Analysis"));
    viewMenu->addAction(analysisAction);
    connect(m_analysisDock, &AnalysisDock::byteEditsRequested, this, &MainWindow::onByteEdits);
    connect(m_analysisDock, &AnalysisDock::byteEditsPreviewRequested, this,
            [this](const QVector<ByteEdit> &edits) {
                if (!m_doc.isOpen())
                    return;
                if (edits.isEmpty()) {
                    m_showingPreview = true; // so showBaseline repaints
                    showBaseline();
                    return;
                }
                QApplication::setOverrideCursor(Qt::WaitCursor);
                QString error;
                const auto rgb = m_doc.previewByteEdits(edits, &error);
                QApplication::restoreOverrideCursor();
                if (!rgb) {
                    log(tr("Preview failed: %1").arg(error));
                    return;
                }
                cancelPreview();
                m_grid->setPixmap(QPixmap::fromImage(rgb->toImage()));
                m_showingPreview = true;
            });
    connect(m_analysisDock, &AnalysisDock::baseMcuRequested, this, &MainWindow::onBaseMcuRequested);
    connect(m_analysisDock, &AnalysisDock::mapChanged, this, [this] {
        m_damage = analysis::DamageMap{};
        if (m_overlay == Overlay::Damage)
            refreshOverlay();
    });
    QAction *logAction = viewMenu->addAction(tr("Show &Log"));
    logAction->setCheckable(true);
    connect(logAction, &QAction::toggled, logDock, &QWidget::setVisible);
    connect(logDock, &QDockWidget::visibilityChanged, logAction, &QAction::setChecked);

    // --- status bar --------------------------------------------------------
    m_hoverLabel = new QLabel(this);
    m_zoomLabel = new QLabel(this);
    statusBar()->addWidget(m_hoverLabel, 1);
    statusBar()->addPermanentWidget(m_zoomLabel);
    connect(m_view, &McuGraphicsView::scaleChanged, this, [this](qreal scale) {
        m_zoomLabel->setText(tr("Zoom %1%").arg(scale * 100.0, 0, 'f', 0));
    });

    // --- preview plumbing --------------------------------------------------
    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    m_previewTimer->setInterval(kPreviewDebounceMs);
    connect(m_previewTimer, &QTimer::timeout, this, &MainWindow::onPreviewTimeout);

    m_previewWatcher = new QFutureWatcher<PreviewResult>(this);
    connect(m_previewWatcher, &QFutureWatcher<PreviewResult>::finished, this,
            &MainWindow::onPreviewReady);

    resize(1400, 900);
}

QWidget *MainWindow::buildStepPanel()
{
    QWidget *panel = new QWidget;
    QVBoxLayout *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    QLabel *hint = new QLabel(tr("Every repair is replayed from the original file in one pass. "
                                 "Untick a step to see the image without it; use Up and Down to "
                                 "reorder (insert and delete do not commute, so order matters). "
                                 "Byte edits always run first."),
                              panel);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    layout->addWidget(hint);

    m_stepList = new QListWidget(panel);
    m_stepList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_stepList->setAlternatingRowColors(true);
    m_stepList->setWordWrap(true);
    m_stepList->setMinimumWidth(280);
    connect(m_stepList, &QListWidget::itemChanged, this, &MainWindow::onStepItemChanged);
    connect(m_stepList, &QListWidget::itemSelectionChanged, this,
            &MainWindow::onStepSelectionChanged);
    layout->addWidget(m_stepList, 1);

    QHBoxLayout *buttons = new QHBoxLayout;
    m_stepUpButton = new QPushButton(tr("Up"), panel);
    m_stepDownButton = new QPushButton(tr("Down"), panel);
    m_stepRemoveButton = new QPushButton(tr("Remove"), panel);
    connect(m_stepUpButton, &QPushButton::clicked, this, &MainWindow::onMoveStepUp);
    connect(m_stepDownButton, &QPushButton::clicked, this, &MainWindow::onMoveStepDown);
    connect(m_stepRemoveButton, &QPushButton::clicked, this, &MainWindow::onRemoveStep);
    buttons->addWidget(m_stepUpButton);
    buttons->addWidget(m_stepDownButton);
    buttons->addStretch(1);
    buttons->addWidget(m_stepRemoveButton);
    layout->addLayout(buttons);

    m_stepHintLabel = new QLabel(panel);
    m_stepHintLabel->setWordWrap(true);
    m_stepHintLabel->setEnabled(false);
    layout->addWidget(m_stepHintLabel);

    return panel;
}

QWidget *MainWindow::buildControlPanel()
{
    QWidget *panel = new QWidget;
    QVBoxLayout *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(10);
    layout->addWidget(buildImageGroup());
    layout->addWidget(buildSelectionGroup());
    layout->addWidget(buildColorGroup());
    layout->addWidget(buildBlockGroup());
    layout->addStretch(1);
    return panel;
}

QGroupBox *MainWindow::buildImageGroup()
{
    QGroupBox *box = new QGroupBox(tr("Image"));
    QVBoxLayout *layout = new QVBoxLayout(box);
    m_infoLabel = new QLabel(box);
    m_infoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_infoLabel->setWordWrap(true);
    layout->addWidget(m_infoLabel);
    return box;
}

QGroupBox *MainWindow::buildSelectionGroup()
{
    QGroupBox *box = new QGroupBox(tr("Selection"));
    QVBoxLayout *layout = new QVBoxLayout(box);

    m_selectionLabel = new QLabel(box);
    m_selectionLabel->setWordWrap(true);
    layout->addWidget(m_selectionLabel);

    m_selectionColorLabel = new QLabel(box);
    m_selectionColorLabel->setWordWrap(true);
    layout->addWidget(m_selectionColorLabel);

    QHBoxLayout *buttons = new QHBoxLayout;
    QPushButton *selectAll = new QPushButton(tr("Select all"), box);
    QPushButton *clear = new QPushButton(tr("Clear"), box);
    connect(selectAll, &QPushButton::clicked, m_selectAllAction, &QAction::trigger);
    connect(clear, &QPushButton::clicked, m_clearSelectionAction, &QAction::trigger);
    buttons->addWidget(selectAll);
    buttons->addWidget(clear);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    QLabel *hint = new QLabel(
        tr("Drag to select a run of MCUs in scan order; Shift+click extends it."), box);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    layout->addWidget(hint);

    return box;
}

QGroupBox *MainWindow::buildColorGroup()
{
    QGroupBox *box = new QGroupBox(tr("Color correction"));
    QVBoxLayout *layout = new QVBoxLayout(box);

    QFormLayout *scopeForm = new QFormLayout;
    m_scopeCombo = new QComboBox(box);
    m_scopeCombo->addItem(tr("Selected MCUs only"));
    m_scopeCombo->addItem(tr("Selection to end of image"));
    m_scopeCombo->addItem(tr("Selection to next restart marker"));
    m_scopeCombo->addItem(tr("Whole image"));
    m_scopeCombo->setToolTip(
        tr("Which MCUs the correction touches. Every option edits DCT coefficients "
           "directly, so MCUs outside the scope decode exactly as they did before.\n\n"
           "A DC error left by a desync persists only until the next restart marker, where "
           "the encoder reset its predictors -- so in a file with restart markers that is "
           "where a DC correction should stop."));
    connect(m_scopeCombo, &QComboBox::currentIndexChanged, this, &MainWindow::onScopeChanged);
    scopeForm->addRow(tr("Scope:"), m_scopeCombo);
    layout->addLayout(scopeForm);

    QGridLayout *sliderGrid = new QGridLayout;
    sliderGrid->setColumnStretch(1, 1);
    for (int c = 0; c < 3; ++c) {
        QLabel *name = new QLabel(QString::fromLatin1(colormath::channelNames[c]), box);

        m_sliders[c] = new QSlider(Qt::Horizontal, box);
        m_sliders[c]->setRange(-colormath::kCdeltaLimit, colormath::kCdeltaLimit);
        m_sliders[c]->setSingleStep(1);
        m_sliders[c]->setPageStep(16);

        m_spins[c] = new QSpinBox(box);
        m_spins[c]->setRange(-colormath::kCdeltaLimit, colormath::kCdeltaLimit);
        m_spins[c]->setAccelerated(true);

        m_shiftLabels[c] = new QLabel(box);
        m_shiftLabels[c]->setMinimumWidth(90);
        m_shiftLabels[c]->setToolTip(tr("The shift this delta produces on 0-255 samples, "
                                        "which depends on this image's DC quantization step."));

        connect(m_sliders[c], &QSlider::valueChanged, m_spins[c], &QSpinBox::setValue);
        connect(m_spins[c], &QSpinBox::valueChanged, m_sliders[c], &QSlider::setValue);
        connect(m_spins[c], &QSpinBox::valueChanged, this, &MainWindow::onDeltasChanged);

        sliderGrid->addWidget(name, c, 0);
        sliderGrid->addWidget(m_sliders[c], c, 1);
        sliderGrid->addWidget(m_spins[c], c, 2);
        sliderGrid->addWidget(m_shiftLabels[c], c, 3);
    }
    layout->addLayout(sliderGrid);

    QHBoxLayout *applyRow = new QHBoxLayout;
    m_applyColorButton = new QPushButton(tr("Apply correction"), box);
    m_resetDeltasButton = new QPushButton(tr("Reset"), box);
    connect(m_applyColorButton, &QPushButton::clicked, this, &MainWindow::onApplyColor);
    connect(m_resetDeltasButton, &QPushButton::clicked, this, &MainWindow::onResetDeltas);
    applyRow->addWidget(m_applyColorButton);
    applyRow->addWidget(m_resetDeltasButton);
    layout->addLayout(applyRow);

    QPushButton *estimate = new QPushButton(tr("Estimate from surroundings"), box);
    estimate->setToolTip(tr("Measures the step in each channel across the edge of the scope, "
                            "where it borders MCUs outside it, and sets the deltas that remove "
                            "it. Works best when the scope starts where the damage starts."));
    connect(estimate, &QPushButton::clicked, this, &MainWindow::onAutoDc);
    layout->addWidget(estimate);

    QFrame *rule = new QFrame(box);
    rule->setFrameShape(QFrame::HLine);
    rule->setFrameShadow(QFrame::Sunken);
    layout->addWidget(rule);

    QLabel *matchHint = new QLabel(
        tr("Match a damaged MCU to good color showing the same content (an MCU of this "
           "image, or a patch of another copy of the photograph):"), box);
    matchHint->setWordWrap(true);
    layout->addWidget(matchHint);

    QHBoxLayout *pickRow = new QHBoxLayout;
    m_pickReferenceButton = new QPushButton(tr("Pick reference"), box);
    m_pickTargetButton = new QPushButton(tr("Pick target"), box);
    m_pickReferenceButton->setCheckable(true);
    m_pickTargetButton->setCheckable(true);
    connect(m_pickReferenceButton, &QPushButton::clicked, this, [this](bool on) {
        m_pickTargetButton->setChecked(false);
        m_grid->setPickMode(on ? McuGridItem::PickMode::Reference : McuGridItem::PickMode::None);
    });
    connect(m_pickTargetButton, &QPushButton::clicked, this, [this](bool on) {
        m_pickReferenceButton->setChecked(false);
        m_grid->setPickMode(on ? McuGridItem::PickMode::Target : McuGridItem::PickMode::None);
    });
    pickRow->addWidget(m_pickReferenceButton);
    pickRow->addWidget(m_pickTargetButton);
    layout->addLayout(pickRow);

    m_referenceFromImageButton = new QPushButton(tr("Reference from another image…"), box);
    m_referenceFromImageButton->setToolTip(
        tr("Measure the reference color in another copy of the same photograph (a surviving "
           "thumbnail, a backup, a gallery cache). Needed when a donor header has left this file "
           "wrong from its very first block, so no block in it can serve as the good one."));
    connect(m_referenceFromImageButton, &QPushButton::clicked, this,
            &MainWindow::onPickReferenceFromImage);
    layout->addWidget(m_referenceFromImageButton);

    m_referenceLabel = new QLabel(box);
    m_referenceLabel->setWordWrap(true);
    m_targetLabel = new QLabel(box);
    m_targetLabel->setWordWrap(true);
    layout->addWidget(m_referenceLabel);
    layout->addWidget(m_targetLabel);

    m_matchButton = new QPushButton(tr("Set deltas from match"), box);
    connect(m_matchButton, &QPushButton::clicked, this, &MainWindow::onMatchColors);
    layout->addWidget(m_matchButton);

    m_matchWarningLabel = new QLabel(box);
    m_matchWarningLabel->setWordWrap(true);
    m_matchWarningLabel->setStyleSheet(warningTextStyle());
    m_matchWarningLabel->hide();
    layout->addWidget(m_matchWarningLabel);

    QFrame *rule2 = new QFrame(box);
    rule2->setFrameShape(QFrame::HLine);
    rule2->setFrameShadow(QFrame::Sunken);
    layout->addWidget(rule2);

    QPushButton *autoColor = new QPushButton(tr("Auto color (levels + midtone contrast)"), box);
    autoColor->setToolTip(tr("A per-channel levels stretch and a midtone contrast curve. It works "
                             "on pixels, so unlike everything else here it re-quantizes every "
                             "MCU -- with this file's own tables and sampling, so the MCU grid and "
                             "every later step stay where they are."));
    connect(autoColor, &QPushButton::clicked, this, &MainWindow::onAutoColor);
    layout->addWidget(autoColor);

    return box;
}

QGroupBox *MainWindow::buildBlockGroup()
{
    QGroupBox *box = new QGroupBox(tr("MCU operations"));
    QVBoxLayout *layout = new QVBoxLayout(box);

    QLabel *hint = new QLabel(tr("Insert and delete shift every MCU from the first selected one "
                                 "to the end of the image, in the order the file codes them. "
                                 "[ and ] preview one MCU at a time, { and } one block; "
                                 "Ctrl+Enter commits, Esc cancels."),
                              box);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    layout->addWidget(hint);

    QHBoxLayout *countRow = new QHBoxLayout;
    m_blockCountSpin = new QSpinBox(box);
    m_blockCountSpin->setRange(1, 100000);
    m_blockCountSpin->setValue(1);
    QPushButton *insert = new QPushButton(tr("Insert"), box);
    QPushButton *remove = new QPushButton(tr("Delete"), box);
    connect(insert, &QPushButton::clicked, this, &MainWindow::onInsertMcus);
    connect(remove, &QPushButton::clicked, this, &MainWindow::onDeleteMcus);
    countRow->addWidget(new QLabel(tr("MCUs:"), box));
    countRow->addWidget(m_blockCountSpin);
    countRow->addWidget(insert);
    countRow->addWidget(remove);
    layout->addLayout(countRow);

    // A stream that lost part of an MCU is out of step by single 8x8 blocks:
    // luma against chroma, which no whole-MCU shift can put back.
    QHBoxLayout *unitRow = new QHBoxLayout;
    m_unitCombo = new QComboBox(box);
    m_unitCombo->setToolTip(tr("Which 8x8 block of the first selected MCU the shift starts at, "
                               "in coding order."));
    m_unitCountSpin = new QSpinBox(box);
    m_unitCountSpin->setRange(1, 100000);
    QPushButton *unitInsert = new QPushButton(tr("Insert"), box);
    QPushButton *unitDelete = new QPushButton(tr("Delete"), box);
    connect(unitInsert, &QPushButton::clicked, this, &MainWindow::onInsertUnits);
    connect(unitDelete, &QPushButton::clicked, this, &MainWindow::onDeleteUnits);
    unitRow->addWidget(new QLabel(tr("Blocks from"), box));
    unitRow->addWidget(m_unitCombo);
    unitRow->addWidget(m_unitCountSpin);
    unitRow->addWidget(unitInsert);
    unitRow->addWidget(unitDelete);
    layout->addLayout(unitRow);

    QPushButton *align = new QPushButton(tr("Find the alignment…"), box);
    align->setToolTip(tr("Tries every shift of the stream at the first selected MCU and ranks them "
                         "by how well the content below continues the content above."));
    connect(align, &QPushButton::clicked, this, &MainWindow::onAutoAlign);
    layout->addWidget(align);

    m_shiftLabel = new QLabel(box);
    m_shiftLabel->setWordWrap(true);
    m_shiftLabel->setStyleSheet(warningTextStyle());
    m_shiftLabel->hide();
    layout->addWidget(m_shiftLabel);

    QFrame *clipLine = new QFrame(box);
    clipLine->setFrameShape(QFrame::HLine);
    clipLine->setFrameShadow(QFrame::Sunken);
    layout->addWidget(clipLine);

    QLabel *clipHint = new QLabel(tr("Copy lifts the selected MCUs; paste puts them back at "
                                     "the selection. Over writes on top of what is there, "
                                     "Inserting pushes the rest of the image along first."),
                                  box);
    clipHint->setWordWrap(true);
    clipHint->setEnabled(false);
    layout->addWidget(clipHint);

    QHBoxLayout *clipRow = new QHBoxLayout;
    QPushButton *clipCopy = new QPushButton(tr("Copy"), box);
    QPushButton *clipPaste = new QPushButton(tr("Paste over"), box);
    QPushButton *clipPasteIns = new QPushButton(tr("Paste inserting"), box);
    clipCopy->setToolTip(m_copySelectionAction->toolTip());
    clipPaste->setToolTip(m_pasteOverAction->toolTip());
    clipPasteIns->setToolTip(m_pasteInsertAction->toolTip());
    connect(clipCopy, &QPushButton::clicked, m_copySelectionAction, &QAction::trigger);
    connect(clipPaste, &QPushButton::clicked, m_pasteOverAction, &QAction::trigger);
    connect(clipPasteIns, &QPushButton::clicked, m_pasteInsertAction, &QAction::trigger);
    clipRow->addWidget(clipCopy);
    clipRow->addWidget(clipPaste);
    clipRow->addWidget(clipPasteIns);
    layout->addLayout(clipRow);

    // The buttons follow their actions, so shortcut and click cannot drift.
    connect(m_copySelectionAction, &QAction::changed, clipCopy,
            [this, clipCopy] { clipCopy->setEnabled(m_copySelectionAction->isEnabled()); });
    connect(m_pasteOverAction, &QAction::changed, clipPaste,
            [this, clipPaste] { clipPaste->setEnabled(m_pasteOverAction->isEnabled()); });
    connect(m_pasteInsertAction, &QAction::changed, clipPasteIns,
            [this, clipPasteIns] { clipPasteIns->setEnabled(m_pasteInsertAction->isEnabled()); });

    m_clipboardLabel = new QLabel(box);
    m_clipboardLabel->setWordWrap(true);
    layout->addWidget(m_clipboardLabel);

    QFrame *offsetLine = new QFrame(box);
    offsetLine->setFrameShape(QFrame::HLine);
    offsetLine->setFrameShadow(QFrame::Sunken);
    layout->addWidget(offsetLine);

    QHBoxLayout *copyRow = new QHBoxLayout;
    m_copyRowSpin = new QSpinBox(box);
    m_copyRowSpin->setRange(-10000, 10000);
    m_copyColSpin = new QSpinBox(box);
    m_copyColSpin->setRange(-10000, 10000);
    QPushButton *copy = new QPushButton(tr("Copy into selection"), box);
    copy->setToolTip(tr("Replaces each selected MCU with the MCU this many rows and columns away."));
    connect(copy, &QPushButton::clicked, this, &MainWindow::onCopyBlocks);
    copyRow->addWidget(new QLabel(tr("ΔRow:"), box));
    copyRow->addWidget(m_copyRowSpin);
    copyRow->addWidget(new QLabel(tr("ΔCol:"), box));
    copyRow->addWidget(m_copyColSpin);
    layout->addLayout(copyRow);
    layout->addWidget(copy);

    QFrame *fillLine = new QFrame(box);
    fillLine->setFrameShape(QFrame::HLine);
    fillLine->setFrameShadow(QFrame::Sunken);
    layout->addWidget(fillLine);

    QLabel *fillHint = new QLabel(tr("Everything above moves coefficients this file already "
                                     "has. Where none survived, content can come from another "
                                     "copy of the photograph instead."),
                                  box);
    fillHint->setWordWrap(true);
    fillHint->setEnabled(false);
    layout->addWidget(fillHint);

    QPushButton *fillReference = new QPushButton(tr("Fill from reference picture…"), box);
    fillReference->setToolTip(m_fillReferenceAction->toolTip());
    connect(fillReference, &QPushButton::clicked, m_fillReferenceAction, &QAction::trigger);
    connect(m_fillReferenceAction, &QAction::changed, fillReference, [this, fillReference] {
        fillReference->setEnabled(m_fillReferenceAction->isEnabled());
    });
    layout->addWidget(fillReference);

    return box;
}

// ---------------------------------------------------------------------------
// File handling
// ---------------------------------------------------------------------------

void MainWindow::onOpen()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open JPEG or repair project"), m_doc.filePath(),
        tr("JPEGs and repair projects (*.jpg *.jpeg *.JPG *.JPEG *.mcup);;"
           "JPEG images (*.jpg *.jpeg *.JPG *.JPEG);;"
           "Repair projects (*.mcup);;All files (*)"));
    if (!path.isEmpty())
        openFile(path);
}

void MainWindow::onOpenWithDonor()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open damaged JPEG with a donor header"), m_doc.filePath(),
        tr("JPEG images (*.jpg *.jpeg *.JPG *.JPEG);;All files (*)"));
    if (!path.isEmpty())
        openWithDonorHeader(path);
}

bool MainWindow::openFile(const QString &path)
{
    if (project::isProjectPath(path))
        return openProject(path);

    // An image with a session beside it opens as that session. This is the
    // whole point of the project file: the user reopens the photograph they
    // were working on, by its own name, and finds the repair where they left
    // it rather than back at the beginning.
    const QString projectPath = project::pathForSource(path);
    if (QFileInfo::exists(projectPath) && openProject(projectPath))
        return true;

    QString error;
    if (!m_doc.load(path, &error))
        return offerDonorHeader(path, error);

    finishOpen();

    const jr::Info &info = m_doc.info();
    log(tr("Opened %1 (%2x%3, %4, %5x%6 MCU grid%7).")
            .arg(QFileInfo(path).fileName())
            .arg(info.width)
            .arg(info.height)
            .arg(info.samplingName())
            .arg(info.mcusX)
            .arg(info.mcusY)
            .arg(info.progressive ? tr(", progressive") : QString()));
    if (!info.interleavedSingleScan()) {
        log(tr("This file spreads its data over %n scan(s). Damage spreads scan by scan rather "
               "than in MCU order here, so insert and delete fix it only when every scan was "
               "affected the same way. The Scans tab of the Analysis panel lists them; dropping "
               "the damaged late scans is often the cleaner fix.",
               nullptr, info.scanCount));
    }
    if (m_doc.trailer().present())
        log(tr("After the image: %1.").arg(m_doc.trailer().describe()));

    reportTrimmedScan(path);
    return true;
}

// Every other complaint this window makes is about something the user can go
// and do about it. This one is not, so it says so plainly rather than letting
// the news scroll past in the log: past the cut there is no damaged data to
// coax back, there is no data, and the tools in the panel work on coefficients
// that have to exist first.
void MainWindow::reportTrimmedScan(const QString &path)
{
    const auto &trim = m_doc.scanTrim();
    if (!trim)
        return;

    const QString name = QFileInfo(path).fileName();

    // Read-through has already been chosen: report what it did and leave the
    // judgement of the result to the person looking at it. This comes first
    // because it drops nothing, and so would otherwise read as the harmless
    // missing-EOI case below.
    if (trim->mode == jr::SalvageMode::ReadThrough) {
        log(tr("Reopened %1 reading through the damage. Defused %2 stray marker(s) from byte "
               "%3 onward.")
                .arg(name)
                .arg(trim->defused)
                .arg(trim->at));
        log(tr("Anything below that point is only a guess at a picture. Treat it as noise "
               "unless it plainly is not."));
        return;
    }

    // A scan running to EOF with no EOI loses nothing (the marker is
    // bookkeeping, not picture), so it stays a log line.
    if (trim->dropped <= 0) {
        log(tr("Picture data ran to the end of the file with no EOI marker. Closed it at "
               "byte %1.")
                .arg(trim->at));
        return;
    }

    const double fraction = trim->lostFraction();
    const QString damaged = QLocale().formattedDataSize(trim->damagedBytes);
    const QString share = fraction > 0
            ? tr("about %1% of its picture data").arg(qRound(fraction * 100))
            : tr("%1 of its picture data").arg(damaged);

    log(tr("Picture data stops being readable at byte %1. Dropped the %2 after it and closed "
           "the file. Blocks past that point have no data to repair.")
            .arg(trim->at)
            .arg(damaged));

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Part of the picture data is missing"));
    box.setText(tr("%1 opened, but %2 is missing.").arg(name, share));
    box.setInformativeText(
        tr("Past that point the file holds garbled data, so there are no blocks left to repair "
           "and the area shows flat gray. What survived is intact and can be worked on "
           "normally.\n\n"
           "This is also why some damaged files open without this warning. A gap filled with "
           "zeros (what a sector-level rescue leaves behind) is still readable as picture "
           "data, so the decoder carries on through it and the loss shows up as flat colored "
           "bands and a picture that slips sideways. The picture is just as gone either way, "
           "and zeros only hide it better. Garbled data stops the decoder outright, which is "
           "what happened to this file.\n\n"
           "Those bytes are not in the file, so another tool is unlikely to do better. If this "
           "came from a memory card you still have, a card-level recovery may find the "
           "photograph whole, which is a better prospect than repairing this copy."));
    // The offsets belong in the dialog, but not in front of someone who only
    // wants to know whether their photograph is coming back.
    box.setDetailedText(tr("Picture data starts at byte %1.\n"
                           "It stops being readable at byte %2.\n"
                           "%3 byte(s) after that were dropped so the file could be opened.\n"
                           "Restart interval: %4")
                            .arg(trim->scanStart)
                            .arg(trim->at)
                            .arg(trim->dropped)
                            .arg(trim->restartInterval > 0
                                         ? tr("%1 MCU(s)").arg(trim->restartInterval)
                                         : tr("none, so there is no resynchronization point")));

    box.addButton(QMessageBox::Ok);
    // One stray byte can silence a stretch that still holds a picture, so the
    // stricter reading is on offer rather than imposed. It is the second button
    // because on most damaged files it finds nothing, and noise that looks like
    // a result is worse than an honest gray.
    QPushButton *readThrough = box.addButton(tr("Try Reading Through"), QMessageBox::ActionRole);
    readThrough->setToolTip(tr("Ignore the stray markers in the damaged stretch and decode to "
                               "the end of the file. Shows noise where nothing survived, but "
                               "reveals picture data that a single bad byte was hiding."));
    box.setDefaultButton(QMessageBox::Ok);
    box.exec();

    if (box.clickedButton() != readThrough)
        return;

    QString error;
    if (!m_doc.load(path, &error, jr::SalvageMode::ReadThrough)) {
        // The file opened a moment ago, so this is worth saying out loud.
        showError(tr("Could not read through the damage"), error);
        return;
    }
    finishOpen();
    reportTrimmedScan(path);
}

// A file that libjpeg refuses is not necessarily beyond reach: if what it
// choked on was the header, another shot from the same camera roll has one
// that fits. Say so where the failure is reported, rather than leaving the
// answer buried in a menu.
bool MainWindow::offerDonorHeader(const QString &path, const QString &whyItFailed)
{
    QByteArray broken;
    if (!readFile(path, &broken)) // readFile has already said why
        return false;

    log(QStringLiteral("%1: %2").arg(tr("Could not open image"), whyItFailed));

    const donor::Layout layout = donor::scan(broken);
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Could not open image"));
    box.setText(tr("%1 could not be opened.").arg(QFileInfo(path).fileName()));
    box.setInformativeText(
        tr("%1\n\n%2\n\nA JPEG's tables and headers sit in front of the picture data, and "
           "nothing in the data can replace them. If you have another photograph from the same "
           "camera roll, shot at the same settings, its header can stand in for this one's.")
            .arg(whyItFailed, donor::describe(layout)));
    QPushButton *useDonor = box.addButton(tr("Use a Donor Header…"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(useDonor);
    box.exec();
    if (box.clickedButton() != useDonor)
        return false;
    return runDonorDialog(path, broken);
}

bool MainWindow::openWithDonorHeader(const QString &path)
{
    QByteArray broken;
    if (!readFile(path, &broken))
        return false;
    return runDonorDialog(path, broken);
}

// Reads a file whole, reporting failure the way the open path does. The donor
// flow needs the damaged bytes in hand before it can say anything useful about
// them, so this happens before any of libjpeg is involved.
bool MainWindow::readFile(const QString &path, QByteArray *out)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        showError(tr("Could not open image"),
                  tr("Could not read %1: %2").arg(path, file.errorString()));
        return false;
    }
    *out = file.readAll();
    file.close();
    if (out->isEmpty()) {
        showError(tr("Could not open image"), tr("%1 is empty.").arg(path));
        return false;
    }
    return true;
}

bool MainWindow::runDonorDialog(const QString &path, const QByteArray &broken)
{
    DonorHeaderDialog dialog(path, broken, this);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    QString error;
    if (!m_doc.loadReconstructed(path, dialog.splicedBytes(), dialog.donorPath(),
                                 dialog.spliceOffset(), &error)) {
        showError(tr("Could not open image"), error);
        return false;
    }
    m_donorOptions = dialog.spliceOptions();

    finishOpen();

    const jr::Info &info = m_doc.info();
    log(tr("Opened %1 with the header from %2 (%3x%4, %5x%6 MCU grid, data resuming at byte %7%8).")
            .arg(QFileInfo(path).fileName(), QFileInfo(dialog.donorPath()).fileName())
            .arg(info.width)
            .arg(info.height)
            .arg(info.mcusX)
            .arg(info.mcusY)
            .arg(dialog.spliceOffset())
            .arg(dialog.carriedExif() ? tr(", keeping the damaged file's own Exif") : QString()));
    log(tr("The picture is very likely shifted: use Find the alignment, or insert and delete "
           "MCUs, to slide the stream into place, and export when it looks right."));
    // A transplanted header does not mend the scan behind it, which may well be
    // cut short too.
    reportTrimmedScan(path);
    return true;
}

void MainWindow::finishOpen(bool startProject)
{
    // Never point Export at whatever was open before, and never at the damaged
    // original: the first export of a session has to ask.
    m_exportPath.clear();
    m_projectPath = project::pathForSource(m_doc.filePath());
    m_projectBroken = false;
    cancelPreview();
    m_showingPreview = false;
    // A reference from another image belonged to the file that was open, not to
    // this one, so a new document starts with nothing picked either way.
    clearPickedBlocks(false);
    m_pickReferenceButton->setChecked(false);
    m_pickTargetButton->setChecked(false);

    loadIntoView();
    onResetDeltas();

    // A donor transplant or a salvaged scan is real work -- a donor found, a
    // splice point swept for, a salvage mode settled on -- and none of it is on
    // disk. Write it down now rather than waiting for a first repair step.
    if (startProject && m_doc.isReconstruction())
        saveProject();
}

// ---------------------------------------------------------------------------
// The project file
// ---------------------------------------------------------------------------

project::Project MainWindow::currentProject() const
{
    project::Project stored;
    stored.sourcePath = m_doc.filePath();
    stored.sourceBytes = QFileInfo(m_doc.filePath()).size();
    stored.sourceSha256 = m_doc.sourceSha256();
    stored.salvageMode = m_doc.salvageMode();
    if (!m_doc.donorPath().isEmpty())
        stored.donor = project::Donor{m_doc.donorPath(), m_doc.donorSpliceOffset(), m_donorOptions};
    stored.exportPath = m_exportPath;
    stored.steps = m_doc.steps();
    stored.history = m_doc.history();
    stored.historyIndex = m_doc.historyIndex();
    return stored;
}

void MainWindow::saveProject()
{
    if (!m_doc.isOpen() || m_projectPath.isEmpty())
        return;

    // Worth saying once, when the file first appears: a tool that writes
    // something next to the user's photographs should say that it does, and
    // this is also where they find out that closing costs them nothing.
    const bool isNew = !QFileInfo::exists(m_projectPath);

    QString error;
    if (project::write(m_projectPath, currentProject(), &error)) {
        m_projectBroken = false;
        if (isNew) {
            log(tr("Keeping this session in %1. It reopens with the image, so the repair "
                   "survives closing the program.")
                    .arg(QFileInfo(m_projectPath).fileName()));
        }
        return;
    }

    // Say this once. The alternative is a dialog after every block inserted,
    // which would train the user to click through the one message here that
    // they cannot afford to miss.
    if (!m_projectBroken) {
        m_projectBroken = true;
        showError(tr("Repairs are not being saved"),
                  tr("%1\n\nThe repair itself is fine and the picture can still be exported, "
                     "but this session is not being written down: if the program stops before "
                     "you export, the steps are lost. This usually means the folder holding "
                     "the image cannot be written to -- read-only rescue media, or a mounted "
                     "disk image. Copying the image somewhere writable and reopening it there "
                     "is the fix.")
                      .arg(error));
    }
    log(error);
}

QString MainWindow::setAsideProject(const QString &projectPath)
{
    if (!QFileInfo::exists(projectPath))
        return QString();

    // Never two sessions deep: keep the one being displaced, but do not build a
    // pile of .bak.bak.bak beside someone's photographs.
    QString kept = projectPath + QStringLiteral(".bak");
    for (int n = 2; QFileInfo::exists(kept) && n < 100; ++n)
        kept = projectPath + QStringLiteral(".bak%1").arg(n);

    return QFile::rename(projectPath, kept) ? kept : QString();
}

bool MainWindow::openProject(const QString &projectPath)
{
    QString error;
    const auto stored = project::read(projectPath, &error);
    if (!stored) {
        // Unreadable or not, it is the only record of a session, and the next
        // repair step would autosave straight over it.
        const QString kept = setAsideProject(projectPath);
        showError(tr("Could not open the repair project"),
                  kept.isEmpty()
                      ? error
                      : tr("%1\n\nIt has been kept as %2, so nothing writes over it.")
                            .arg(error, QFileInfo(kept).fileName()));
        return false;
    }

    const QFileInfo sourceInfo(stored->sourcePath);
    if (!sourceInfo.exists()) {
        showError(tr("Could not open the repair project"),
                  tr("%1 repairs %2, which is not there any more.\n\nThe project holds the "
                     "repair steps, not the picture, so the image it was built from has to be "
                     "in place. If it was moved, put the project beside it and open it again.")
                      .arg(QFileInfo(projectPath).fileName(),
                           QDir::toNativeSeparators(stored->sourcePath)));
        return false;
    }

    if (stored->donor) {
        // Rebuild the same transplant: the reconstruction was never on disk, and
        // the steps were built against its exact bytes.
        QByteArray broken;
        if (!readFile(stored->sourcePath, &broken))
            return false;
        QByteArray donorBytes;
        if (!readFile(stored->donor->path, &donorBytes))
            return false;

        const donor::Layout layout = donor::scan(donorBytes);
        auto spliced = donor::splice(donorBytes, layout, broken, stored->donor->spliceOffset,
                                     &error, stored->donor->options);
        if (!spliced) {
            showError(tr("Could not rebuild the donor header"),
                      tr("%1 was opened with a header borrowed from %2, and that header can no "
                         "longer be used: %3")
                          .arg(sourceInfo.fileName(),
                               QFileInfo(stored->donor->path).fileName(), error));
            return false;
        }
        if (!m_doc.loadReconstructed(stored->sourcePath, spliced->bytes, stored->donor->path,
                                     stored->donor->spliceOffset, &error)) {
            showError(tr("Could not open image"), error);
            return false;
        }
    } else if (!m_doc.load(stored->sourcePath, &error, stored->salvageMode)) {
        showError(tr("Could not open image"), error);
        return false;
    }

    finishOpen(false);
    m_projectPath = projectPath;
    m_exportPath = stored->exportPath;
    m_donorOptions = stored->donor ? stored->donor->options : donor::SpliceOptions{};

    log(tr("Resumed the repair of %1 from %2.")
            .arg(sourceInfo.fileName(), QFileInfo(projectPath).fileName()));
    if (!stored->sourceSha256.isEmpty() && !m_doc.sourceSha256().isEmpty()
        && stored->sourceSha256 != m_doc.sourceSha256()) {
        log(tr("Careful: %1 is not the file this repair was made on (its SHA-256 differs). The "
               "steps were worked out against different bytes.")
                .arg(sourceInfo.fileName()));
    } else if (stored->sourceSha256.isEmpty() && stored->sourceBytes > 0
               && stored->sourceBytes != sourceInfo.size()) {
        log(tr("Careful: %1 is %2 byte(s) now and was %3 when the repair was made. The steps "
               "were worked out against different bytes.")
                .arg(sourceInfo.fileName())
                .arg(sourceInfo.size())
                .arg(stored->sourceBytes));
    }

    if (!stored->steps.isEmpty()) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool replayed =
            m_doc.adoptSteps(stored->steps, &error, stored->history, stored->historyIndex);
        QApplication::restoreOverrideCursor();

        if (!replayed) {
            // The steps replayed once, when they were made. That they will not
            // now means the file underneath them is not the one they were built
            // against, and there is nothing to be done about it here -- but the
            // record of what was tried is worth keeping.
            const QString kept = setAsideProject(projectPath);
            showError(tr("Could not replay the repair"),
                      tr("%1 opened, but the %n saved repair step(s) could not be applied to "
                         "it: %2\n\nThe image is open as it stands on disk, with no repairs. "
                         "The steps have been kept in %3.",
                         nullptr, stored->steps.size())
                          .arg(sourceInfo.fileName(), error,
                               kept.isEmpty() ? QFileInfo(projectPath).fileName()
                                              : QFileInfo(kept).fileName()));
            return true;
        }
        refreshFromDocument();
        log(tr("Replayed %n repair step(s).", nullptr, stored->steps.size()));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

void MainWindow::onExport()
{
    if (!requireImage())
        return;
    // The damaged original is never the default target: it is the one thing
    // that cannot be regenerated if a repair goes wrong. So the first export
    // asks, and every one after it goes where that one went.
    if (m_exportPath.isEmpty()) {
        onExportAs();
        return;
    }
    doExport(m_exportPath);
}

bool MainWindow::doExport(const QString &path)
{
    QString error;
    ImageDocument::ExportOptions options;
    options.refreshThumbnail = m_thumbnailAction->isChecked();
    options.keepTrailer = m_trailerAction->isChecked();
    if (!m_doc.exportTo(path, &error, options)) {
        showError(tr("Could not export"), error);
        return false;
    }
    log(tr("Exported %1.").arg(path));
    if (m_reportAction->isChecked()) {
        QFile written(path);
        if (written.open(QIODevice::ReadOnly)) {
            const QJsonObject r = report::build(m_doc, path, written.readAll());
            if (report::writeBeside(path, r, &error))
                log(tr("Wrote the recovery report beside it."));
            else
                log(tr("Could not write the recovery report: %1").arg(error));
        }
    }
    updateWindowTitle();
    return true;
}

void MainWindow::onExportAs()
{
    if (!requireImage())
        return;

    QString suggestion = m_exportPath;
    if (suggestion.isEmpty()) {
        const QFileInfo source(m_doc.filePath());
        // Ransomware appends its own extension (photo.jpg.xyzw); the export
        // should be a .jpg whatever the damaged file is called.
        QString base = source.completeBaseName();
        const qsizetype jpg = base.lastIndexOf(QStringLiteral(".jp"), -1, Qt::CaseInsensitive);
        if (jpg > 0)
            base = base.left(jpg);
        suggestion = source.dir().filePath(base + QStringLiteral("_repaired.jpg"));
    }

    const QString path = QFileDialog::getSaveFileName(this, tr("Export repaired JPEG"), suggestion,
                                                      tr("JPEG images (*.jpg *.jpeg)"));
    if (path.isEmpty())
        return;
    if (!doExport(path))
        return;
    m_exportPath = path;
    // Where the export went is part of the session: reopening the project and
    // pressing Export should go on writing to the same file.
    saveProject();
    updateWindowTitle();
}

void MainWindow::onWriteReport()
{
    if (!requireImage())
        return;
    QString error;
    const auto bytes = m_doc.exportBytes(&error);
    if (!bytes) {
        showError(tr("Could not build the report"), error);
        return;
    }
    const QString target = m_exportPath.isEmpty() ? m_doc.filePath() + QStringLiteral(".repaired.jpg")
                                                  : m_exportPath;
    const QJsonObject r = report::build(m_doc, target, *bytes);
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Write recovery report"), target + QStringLiteral(".report.txt"),
        tr("Text (*.txt);;JSON (*.json)"));
    if (path.isEmpty())
        return;
    QSaveFile f(path);
    const QByteArray out = path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive)
        ? QJsonDocument(r).toJson(QJsonDocument::Indented)
        : report::toText(r).toUtf8();
    if (!f.open(QIODevice::WriteOnly) || f.write(out) != out.size() || !f.commit()) {
        showError(tr("Could not write the report"), f.errorString());
        return;
    }
    log(tr("Wrote %1.").arg(path));
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    // The desktop toggled light<->dark while we're running. Standard widgets
    // re-read the new palette on their own; the warning label's color is a
    // fixed hex chosen for the old scheme, so re-pick it. (ApplicationPaletteChange
    // is the usual signal; ThemeChange covers the same on some platforms. Both
    // can arrive before buildUi() has run.)
    if ((event->type() == QEvent::ApplicationPaletteChange
         || event->type() == QEvent::ThemeChange)
        && m_matchWarningLabel)
        m_matchWarningLabel->setStyleSheet(warningTextStyle());
}

// Closing asks nothing in the ordinary case, and that is the point of the
// project file: the repair is already on disk, and reopening the image comes
// back to exactly this. The one thing worth stopping for is a session that was
// never written down, because then closing really does throw the work away.
void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!m_doc.isOpen() || !m_projectBroken || !m_doc.hasUnexportedChanges()) {
        event->accept();
        return;
    }
    const auto choice = QMessageBox::warning(
        this, tr("Repairs were not saved"),
        tr("The repairs to %1 could not be written to a project file, and have not been "
           "exported either. Closing now loses them. Close anyway?")
            .arg(m_doc.fileName()),
        QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    event->setAccepted(choice == QMessageBox::Discard);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    const QMimeData *mime = event->mimeData();
    if (mime->hasUrls() && mime->urls().size() == 1 && mime->urls().first().isLocalFile())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.size() != 1)
        return;
    if (openFile(urls.first().toLocalFile()))
        event->acceptProposedAction();
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------

void MainWindow::onUndo()
{
    if (!m_doc.undo())
        return;
    cancelPreview();
    refreshFromDocument();
    log(tr("Undo."));
    schedulePreview();
}

void MainWindow::onRedo()
{
    if (!m_doc.redo())
        return;
    cancelPreview();
    refreshFromDocument();
    log(tr("Redo."));
    schedulePreview();
}

void MainWindow::onResetToOriginal()
{
    if (!requireImage())
        return;
    QString error;
    if (!m_doc.clearSteps(&error)) {
        log(error.isEmpty() ? tr("Already showing the original.") : error);
        return;
    }
    cancelPreview();
    refreshFromDocument();
    log(tr("Cleared every repair step (undoable)."));
    schedulePreview();
}

// ---------------------------------------------------------------------------
// The step list
// ---------------------------------------------------------------------------

void MainWindow::applyStepChange(bool ok, const QString &error, const QString &description)
{
    cancelPreview();
    m_showingPreview = false;
    if (!ok) {
        // The recipe is unchanged, but the list may be showing the edit the
        // user just made, so put it back in step with the document.
        refreshStepList();
        log(error.isEmpty() ? tr("That change could not be applied.") : error);
        return;
    }
    refreshFromDocument();
    log(description);
    schedulePreview();
}

void MainWindow::onStepItemChanged(QListWidgetItem *item)
{
    if (m_updatingSteps || !item)
        return;
    const int row = m_stepList->row(item);
    if (row < 0 || row >= m_doc.stepCount())
        return;

    const bool enabled = item->checkState() == Qt::Checked;
    if (enabled == m_doc.steps().at(row).enabled)
        return;

    QString error;
    const bool ok = m_doc.setStepEnabled(row, enabled, &error);
    applyStepChange(ok, error,
                    enabled ? tr("Enabled step %1: %2")
                                  .arg(row + 1)
                                  .arg(m_doc.steps().at(row).description)
                            : tr("Disabled step %1: %2")
                                  .arg(row + 1)
                                  .arg(m_doc.steps().at(row).description));
}

void MainWindow::onStepSelectionChanged()
{
    updateActionStates();
}

void MainWindow::onRemoveStep()
{
    const int row = m_stepList->currentRow();
    if (row < 0 || row >= m_doc.stepCount())
        return;
    const QString description = m_doc.steps().at(row).description;
    QString error;
    const bool ok = m_doc.removeStep(row, &error);
    applyStepChange(ok, error, tr("Removed step %1: %2").arg(row + 1).arg(description));
    if (ok)
        m_stepList->setCurrentRow(qMin(row, m_doc.stepCount() - 1));
}

void MainWindow::onMoveStepUp()
{
    const int row = m_stepList->currentRow();
    if (row <= 0)
        return;
    QString error;
    const bool ok = m_doc.moveStep(row, row - 1, &error);
    applyStepChange(ok, error, tr("Moved step %1 up.").arg(row + 1));
    if (ok)
        m_stepList->setCurrentRow(row - 1);
}

void MainWindow::onMoveStepDown()
{
    const int row = m_stepList->currentRow();
    if (row < 0 || row + 1 >= m_doc.stepCount())
        return;
    QString error;
    const bool ok = m_doc.moveStep(row, row + 1, &error);
    applyStepChange(ok, error, tr("Moved step %1 down.").arg(row + 1));
    if (ok)
        m_stepList->setCurrentRow(row + 1);
}

void MainWindow::refreshStepList()
{
    // Rebuilding fires itemChanged per row; the guard keeps that from looping
    // back into the document.
    m_updatingSteps = true;
    const int previousRow = m_stepList->currentRow();
    m_stepList->clear();

    const QVector<RepairStep> &steps = m_doc.steps();
    for (int i = 0; i < steps.size(); ++i) {
        const RepairStep &step = steps.at(i);
        QListWidgetItem *item = new QListWidgetItem(
            tr("%1. %2").arg(i + 1).arg(step.description), m_stepList);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(step.enabled ? Qt::Checked : Qt::Unchecked);
        if (step.kind == RepairStep::Kind::AutoColor) {
            item->setToolTip(tr("Pixel-domain, so this step re-encodes. Steps after it are "
                                "applied to the re-encoded image."));
        }
        if (!step.enabled) {
            QFont font = item->font();
            font.setItalic(true);
            item->setFont(font);
        }
    }
    if (previousRow >= 0 && previousRow < m_stepList->count())
        m_stepList->setCurrentRow(previousRow);
    m_updatingSteps = false;

    if (steps.isEmpty()) {
        m_stepHintLabel->setText(m_doc.isOpen()
                                     ? tr("No steps yet. The original file as it was opened.")
                                     : tr("No image loaded."));
    } else {
        int enabled = 0;
        for (const RepairStep &step : steps)
            enabled += step.enabled ? 1 : 0;
        m_stepHintLabel->setText(tr("%1 step(s), %2 enabled.").arg(steps.size()).arg(enabled));
    }
}

// ---------------------------------------------------------------------------
// Selection and hover
// ---------------------------------------------------------------------------

void MainWindow::onSelectionChanged()
{
    // A live shift belongs to where it was started.
    if (m_pendingMcuShift || m_pendingUnitShift)
        cancelShift();
    updateSelectionInfo();
    updateActionStates();
    if (m_hoverIndex < 0) {
        int row = 0, col = 0;
        m_analysisDock->setCurrentMcu(m_grid->anchorBlock(&row, &col) ? row * m_doc.info().mcusX + col : -1);
    }
    // The selection defines the scope, so a pending correction now covers
    // different blocks.
    schedulePreview();
}

void MainWindow::onBlockHovered(int row, int col)
{
    if (row < 0 || !m_doc.isOpen()) {
        m_hoverLabel->clear();
        m_hoverIndex = -1;
        int r = 0, c = 0;
        if (m_doc.isOpen())
            m_analysisDock->setCurrentMcu(m_grid->anchorBlock(&r, &c) ? r * m_doc.info().mcusX + c : -1);
        return;
    }
    m_hoverIndex = row * m_doc.info().mcusX + col;
    m_analysisDock->setCurrentMcu(m_hoverIndex);

    const QRect rect = colormath::mcuRect(m_doc.info(), row, col);
    QString text = tr("MCU row %1, col %2   ·   pixels (%3, %4)–(%5, %6)")
                       .arg(row)
                       .arg(col)
                       .arg(rect.left())
                       .arg(rect.top())
                       .arg(rect.right())
                       .arg(rect.bottom());

    const colormath::BlockStats stats = colormath::measure(m_doc.ycbcr(), rect);
    if (stats.isValid())
        text += QStringLiteral("   ·   ") + formatTriple(stats.mean);
    if (m_damage.isValid() && m_hoverIndex < m_damage.score.size() && m_damage.score[m_hoverIndex] >= 0.5f)
        text += tr("   ·   looks damaged");
    m_hoverLabel->setText(text);
}

void MainWindow::onBlockPicked(McuGridItem::PickMode mode, int row, int col)
{
    if (!m_doc.isOpen())
        return;

    const colormath::BlockStats stats =
        colormath::measure(m_doc.ycbcr(), colormath::mcuRect(m_doc.info(), row, col));
    if (!stats.isValid()) {
        log(tr("Could not measure that MCU."));
        return;
    }

    if (mode == McuGridItem::PickMode::Reference) {
        m_referenceStats = stats;
        m_reference = Reference{Reference::Kind::Block, row, col, QString(), QRect()};
        m_pickReferenceButton->setChecked(false);
    } else {
        m_targetStats = stats;
        m_targetRow = row;
        m_targetCol = col;
        m_pickTargetButton->setChecked(false);
    }
    m_grid->setMarkedBlock(mode, row, col);
    updateMatchInfo();
}

// ---------------------------------------------------------------------------
// Color correction
// ---------------------------------------------------------------------------

MainWindow::ScopeChoice MainWindow::scopeChoice() const
{
    return static_cast<ScopeChoice>(m_scopeCombo->currentIndex());
}

jr::Scope MainWindow::currentScope() const
{
    const jr::Info &info = m_doc.info();
    switch (scopeChoice()) {
    case ScopeChoice::SelectedBlocks:
        return jr::Scope::mask(m_grid->selectionMask(), info.mcusY, info.mcusX);
    case ScopeChoice::SelectionToEnd: {
        int row = 0, col = 0;
        m_grid->anchorBlock(&row, &col);
        return jr::Scope::runFrom(row, col);
    }
    case ScopeChoice::SelectionToRestart: {
        int row = 0, col = 0;
        m_grid->anchorBlock(&row, &col);
        const int ri = info.restartInterval;
        if (ri <= 0)
            return jr::Scope::runFrom(row, col);
        const int start = row * info.mcusX + col;
        const int end = (start / ri + 1) * ri; // the first MCU after the next restart
        return jr::Scope::runFrom(row, col, end - start);
    }
    case ScopeChoice::WholeImage:
    default:
        return jr::Scope::wholeImage();
    }
}

QByteArray MainWindow::currentScopeMask() const
{
    const jr::Info &info = m_doc.info();
    if (!info.isValid())
        return QByteArray();

    switch (scopeChoice()) {
    case ScopeChoice::SelectedBlocks:
        return m_grid->selectionMask();
    case ScopeChoice::SelectionToEnd:
    case ScopeChoice::SelectionToRestart: {
        int row = 0, col = 0;
        if (!m_grid->anchorBlock(&row, &col))
            return QByteArray();
        QByteArray mask(qsizetype(info.mcusY) * info.mcusX, '\0');
        const qsizetype from = qsizetype(row) * info.mcusX + col;
        qsizetype to = mask.size();
        if (scopeChoice() == ScopeChoice::SelectionToRestart && info.restartInterval > 0)
            to = qMin(to, (from / info.restartInterval + 1) * info.restartInterval);
        for (qsizetype i = from; i < to; ++i)
            mask[i] = 1;
        return mask;
    }
    case ScopeChoice::WholeImage:
    default:
        return QByteArray(qsizetype(info.mcusY) * info.mcusX, '\1');
    }
}

int MainWindow::deltaFor(int component) const
{
    return m_spins[component] ? m_spins[component]->value() : 0;
}

QVector<jr::Op> MainWindow::pendingShiftOps() const
{
    QVector<jr::Op> ops;
    int row = 0, col = 0;
    if (!m_doc.isOpen() || !m_grid->anchorBlock(&row, &col))
        return ops;
    const int unit = m_unitCombo ? qMax(0, m_unitCombo->currentIndex()) : 0;
    if (m_pendingUnitShift > 0)
        ops.append(jr::Op::insertUnits(m_pendingUnitShift, row, col, unit));
    else if (m_pendingUnitShift < 0)
        ops.append(jr::Op::deleteUnits(-m_pendingUnitShift, row, col, unit));
    if (m_pendingMcuShift > 0)
        ops.append(jr::Op::insertMcus(m_pendingMcuShift, jr::Scope::runFrom(row, col)));
    else if (m_pendingMcuShift < 0)
        ops.append(jr::Op::deleteMcus(-m_pendingMcuShift, jr::Scope::runFrom(row, col)));
    return ops;
}

QVector<jr::Op> MainWindow::pendingOps() const
{
    return pendingShiftOps() + pendingColorOps();
}

bool MainWindow::hasPendingEdit() const
{
    return hasPendingColorEdit() || ((m_pendingMcuShift || m_pendingUnitShift) && m_grid->hasSelection());
}

void MainWindow::nudgeShift(int mcus, int units)
{
    if (!m_doc.isOpen() || !requireSelection())
        return;
    m_pendingMcuShift += mcus;
    m_pendingUnitShift += units;
    const QString mcuText = m_pendingMcuShift > 0 ? tr("insert %n MCU(s)", nullptr, m_pendingMcuShift)
        : m_pendingMcuShift < 0                    ? tr("delete %n MCU(s)", nullptr, -m_pendingMcuShift)
                                                   : QString();
    const QString unitText = m_pendingUnitShift > 0 ? tr("insert %n block(s)", nullptr, m_pendingUnitShift)
        : m_pendingUnitShift < 0                     ? tr("delete %n block(s)", nullptr, -m_pendingUnitShift)
                                                     : QString();
    QStringList parts;
    if (!unitText.isEmpty())
        parts << unitText;
    if (!mcuText.isEmpty())
        parts << mcuText;
    if (parts.isEmpty()) {
        cancelShift();
        return;
    }
    m_shiftLabel->setProperty("what", parts.join(QStringLiteral(", ")));
    m_shiftLabel->setText(tr("Previewing: %1. Ctrl+Enter commits, Esc cancels.")
                              .arg(parts.join(QStringLiteral(", "))));
    m_shiftLabel->show();
    schedulePreview();
}

void MainWindow::commitShift()
{
    const QVector<jr::Op> ops = pendingShiftOps();
    if (ops.isEmpty())
        return;
    int row = 0, col = 0;
    m_grid->anchorBlock(&row, &col);
    const QString what = m_shiftLabel->property("what").toString();
    m_pendingMcuShift = m_pendingUnitShift = 0;
    m_shiftLabel->hide();
    commit(ops, tr("Shifted at row %1, col %2: %3.").arg(row).arg(col).arg(what));
}

void MainWindow::cancelShift()
{
    if (!m_pendingMcuShift && !m_pendingUnitShift)
        return;
    m_pendingMcuShift = m_pendingUnitShift = 0;
    m_shiftLabel->hide();
    schedulePreview();
}

bool MainWindow::hasPendingColorEdit() const
{
    if (!m_doc.isOpen())
        return false;
    if (scopeChoice() != ScopeChoice::WholeImage && !m_grid->hasSelection())
        return false;
    return deltaFor(0) != 0 || deltaFor(1) != 0 || deltaFor(2) != 0;
}

QVector<jr::Op> MainWindow::pendingColorOps() const
{
    QVector<jr::Op> ops;
    if (!hasPendingColorEdit())
        return ops;

    const jr::Scope scope = currentScope();
    for (int c = 0; c < 3; ++c) {
        const int delta = deltaFor(c);
        if (delta != 0)
            ops.append(jr::Op::cdelta(c, delta, scope));
    }
    return ops;
}

void MainWindow::onDeltasChanged()
{
    if (m_updatingControls)
        return;
    updateDeltaLabels();
    updateActionStates();
    schedulePreview();
}

void MainWindow::onScopeChanged()
{
    updateActionStates();
    schedulePreview();
}

void MainWindow::onResetDeltas()
{
    m_updatingControls = true;
    for (int c = 0; c < 3; ++c) {
        m_sliders[c]->setValue(0);
        m_spins[c]->setValue(0);
    }
    m_updatingControls = false;
    updateDeltaLabels();
    updateActionStates();
    cancelPreview();
    showBaseline();
}

void MainWindow::onPickReferenceFromImage()
{
    if (!requireImage())
        return;

    const QString start = QFileInfo(m_doc.filePath()).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose an image to take the reference color from"), start,
        tr("Images (*.jpg *.jpeg *.JPG *.JPEG *.png *.bmp *.tif *.tiff *.webp);;All files (*)"));
    if (path.isEmpty())
        return;

    const jr::Info &info = m_doc.info();
    // The target block, so the dialog can offer to put the patch where that
    // block falls in the other copy.
    const QRect targetRect = m_targetStats
                                 ? colormath::mcuRect(info, m_targetRow, m_targetCol)
                                 : QRect();
    const QString targetName = m_targetStats
                                   ? tr("the target block (row %1, col %2)")
                                         .arg(m_targetRow)
                                         .arg(m_targetCol)
                                   : QString();

    ReferenceColorDialog dialog(path, QSize(info.width, info.height), targetRect, targetName,
                                 this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    m_referenceStats = dialog.stats();
    m_reference = Reference{Reference::Kind::External, -1,           -1,
                            dialog.referencePath(),    dialog.patch(), dialog.monochrome()};
    // Nothing in this image is the reference any more, so the mark that said
    // one of its blocks was has to go.
    m_pickReferenceButton->setChecked(false);
    if (m_grid->pickMode() == McuGridItem::PickMode::Reference)
        m_grid->setPickMode(McuGridItem::PickMode::None);
    m_grid->setMarkedBlock(McuGridItem::PickMode::Reference, -1, -1);

    updateMatchInfo();
    log(tr("Reference color taken from %1: %2.")
            .arg(describeReferenceSource(), formatTriple(m_referenceStats->mean)));
}

void MainWindow::onMatchColors()
{
    if (!m_referenceStats || !m_targetStats) {
        QMessageBox::information(this, tr("Pick a reference and a target"),
                                 tr("Pick good color to match (a block of this image, or a "
                                    "patch of another copy of the photograph) and a target "
                                    "block showing the damage, then try again."));
        return;
    }

    const jr::Info &info = m_doc.info();
    m_updatingControls = true;
    for (int c = 0; c < 3; ++c) {
        const double pixelDelta = m_referenceStats->mean[c] - m_targetStats->mean[c];
        const int delta = colormath::cdeltaForPixelShift(info.dcQuant[c], pixelDelta);
        m_sliders[c]->setValue(delta);
        m_spins[c]->setValue(delta);
    }
    m_updatingControls = false;

    updateDeltaLabels();
    updateActionStates();
    log(tr("Match from target (row %1, col %2) to reference (%3): Y %4, Cb %5, Cr %6.")
            .arg(m_targetRow)
            .arg(m_targetCol)
            .arg(describeReferenceSource())
            .arg(deltaFor(0))
            .arg(deltaFor(1))
            .arg(deltaFor(2)));
    schedulePreview();
}

void MainWindow::onApplyColor()
{
    if (!requireImage())
        return;
    if (scopeChoice() != ScopeChoice::WholeImage && !requireSelection())
        return;

    const QVector<jr::Op> ops = pendingColorOps();
    if (ops.isEmpty()) {
        QMessageBox::information(this, tr("Nothing to apply"),
                                 tr("All three deltas are zero."));
        return;
    }

    const QString description = tr("Applied Y %1, Cb %2, Cr %3 to %4.")
                                    .arg(deltaFor(0))
                                    .arg(deltaFor(1))
                                    .arg(deltaFor(2))
                                    .arg(m_scopeCombo->currentText().toLower());
    if (commit(ops, description))
        onResetDeltas();
}

void MainWindow::onAutoColor()
{
    if (!requireImage())
        return;

    const auto choice = QMessageBox::question(
        this, tr("Re-encode the image?"),
        tr("Auto color works on pixels, not DCT coefficients, so applying it re-quantizes every "
           "MCU of the image -- with this file's own quantization tables and sampling, so the "
           "grid and the steps after it stay put, but it is a generation of loss all the same.\n\n"
           "Every other repair here is lossless. Continue?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (choice != QMessageBox::Yes)
        return;

    const QString description =
        tr("Auto color (levels + midtone contrast), re-quantized with this file's tables");

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const bool ok = m_doc.addAutoColor(description, &error);
    QApplication::restoreOverrideCursor();

    if (!ok) {
        showError(tr("Auto color failed"), error);
        return;
    }

    cancelPreview();
    m_showingPreview = false;
    refreshFromDocument();
    log(description + QStringLiteral("."));
}

// ---------------------------------------------------------------------------
// Block operations
// ---------------------------------------------------------------------------

void MainWindow::onInsertMcus()
{
    if (!requireImage() || !requireSelection())
        return;
    int row = 0, col = 0;
    m_grid->anchorBlock(&row, &col);
    const int n = m_blockCountSpin->value();
    commit({jr::Op::insertMcus(n, jr::Scope::runFrom(row, col))},
           tr("Inserted %n MCU(s) at row %1, col %2.", nullptr, n).arg(row).arg(col));
}

void MainWindow::onDeleteMcus()
{
    if (!requireImage() || !requireSelection())
        return;
    int row = 0, col = 0;
    m_grid->anchorBlock(&row, &col);
    const int n = m_blockCountSpin->value();
    commit({jr::Op::deleteMcus(n, jr::Scope::runFrom(row, col))},
           tr("Deleted %n MCU(s) at row %1, col %2.", nullptr, n).arg(row).arg(col));
}

void MainWindow::onInsertUnits()
{
    if (!requireImage() || !requireSelection())
        return;
    int row = 0, col = 0;
    m_grid->anchorBlock(&row, &col);
    const int n = m_unitCountSpin->value();
    const int unit = qMax(0, m_unitCombo->currentIndex());
    commit({jr::Op::insertUnits(n, row, col, unit)},
           tr("Inserted %n block(s) before block %1 of row %2, col %3.", nullptr, n)
               .arg(m_doc.info().unitName(unit))
               .arg(row)
               .arg(col));
}

void MainWindow::onDeleteUnits()
{
    if (!requireImage() || !requireSelection())
        return;
    int row = 0, col = 0;
    m_grid->anchorBlock(&row, &col);
    const int n = m_unitCountSpin->value();
    const int unit = qMax(0, m_unitCombo->currentIndex());
    commit({jr::Op::deleteUnits(n, row, col, unit)},
           tr("Deleted %n block(s) from block %1 of row %2, col %3.", nullptr, n)
               .arg(m_doc.info().unitName(unit))
               .arg(row)
               .arg(col));
}

void MainWindow::onCopyBlocks()
{
    if (!requireImage() || !requireSelection())
        return;
    const int dRow = m_copyRowSpin->value();
    const int dCol = m_copyColSpin->value();
    if (dRow == 0 && dCol == 0) {
        QMessageBox::information(this, tr("Nothing to copy"),
                                 tr("A zero offset would copy every MCU onto itself."));
        return;
    }
    commit({jr::Op::copyBlocks(dRow, dCol, currentScope())},
           tr("Copied MCUs from offset row %1, col %2 into the selection.").arg(dRow).arg(dCol));
}

void MainWindow::onCopySelection()
{
    if (!requireImage() || !requireSelection())
        return;
    int row = 0, col = 0;
    m_grid->anchorBlock(&row, &col);

    QString error;
    const std::optional<jr::Clipboard> clip =
        m_doc.coefs()->readMcus(row, col, m_grid->selectedCount(), &error);
    if (!clip) {
        showError(tr("Copy failed"), error);
        return;
    }

    m_clipboard = *clip;
    m_clipboardSource = m_doc.filePath();
    updateClipboardInfo();
    updateActionStates();
    log(tr("Copied %n MCU(s) from row %1, col %2.", nullptr, m_clipboard.mcuCount)
            .arg(row)
            .arg(col));
}

void MainWindow::onPasteOver()
{
    pasteClipboard(false);
}

void MainWindow::onPasteInsert()
{
    pasteClipboard(true);
}

void MainWindow::pasteClipboard(bool insert)
{
    if (!requireImage() || !requireSelection())
        return;
    if (!m_clipboard.fits(m_doc.info())) {
        showError(tr("Cannot paste"),
                  tr("The clipboard holds MCUs from an image with different chroma sampling (%1 "
                     "here). Pasting them would pull the color channels out of step.")
                      .arg(m_doc.info().samplingName()));
        return;
    }
    jr::Clipboard clip = m_clipboard;
    const QVector<quint16> tables = m_doc.coefs()->quantTables();
    if (!clip.sameQuantization(tables)) {
        // Coefficients are quantized by the table they were written with, so
        // the same numbers mean different amounts here. Converting them is
        // lossy only where this file's steps are coarser.
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr("Paste from another file"));
        box.setText(tr("These MCUs were copied from %1, which uses different quantization tables.")
                        .arg(QFileInfo(m_clipboardSource).fileName()));
        box.setInformativeText(
            tr("Pasted as they are, the same numbers would mean different amounts here and the area "
               "would come out at the wrong brightness or color. Converting re-expresses every "
               "coefficient in this file's tables -- lossy only where this file's steps are coarser "
               "than the source's."));
        QPushButton *convert = box.addButton(tr("Convert and Paste"), QMessageBox::AcceptRole);
        QPushButton *raw = box.addButton(tr("Paste As Is"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(convert);
        box.exec();
        if (box.clickedButton() == convert)
            clip = clip.requantized(tables);
        else if (box.clickedButton() != raw)
            return;
    }

    int row = 0, col = 0;
    m_grid->anchorBlock(&row, &col);
    const jr::Info &info = m_doc.info();
    const int room = info.mcuCount() - (row * info.mcusX + col);
    if (clip.mcuCount > room) {
        QMessageBox::information(this, tr("Not enough room"),
                                 tr("The clipboard holds %1 MCU(s) but only %2 fit between "
                                    "here and the end of the image. The overflow will be "
                                    "dropped.")
                                     .arg(clip.mcuCount)
                                     .arg(room));
    }

    QVector<jr::Op> ops;
    if (insert) {
        // Open the hole first, then fill it. Insert duplicates MCUs at the
        // seam; the paste lands on exactly those, so the duplicates never show.
        ops.push_back(jr::Op::insertMcus(clip.mcuCount, jr::Scope::runFrom(row, col)));
    }
    ops.push_back(jr::Op::paste(row, col, clip));

    if (!commit(ops,
                insert
                    ? tr("Pasted %1 MCU(s) at row %2, col %3, pushing the rest of the image "
                         "along.")
                          .arg(clip.mcuCount)
                          .arg(row)
                          .arg(col)
                    : tr("Pasted %1 MCU(s) over row %2, col %3.")
                          .arg(clip.mcuCount)
                          .arg(row)
                          .arg(col)))
        return;

    // Leave the pasted run selected, so it can be looked over or corrected
    // without hunting for it again.
    const int last = qMin(row * info.mcusX + col + m_clipboard.mcuCount - 1, info.mcuCount() - 1);
    m_grid->selectRange(row, col, last / info.mcusX, last % info.mcusX);
}

void MainWindow::onFillFromReference()
{
    if (!requireImage())
        return;
    if (scopeChoice() != ScopeChoice::WholeImage && !requireSelection())
        return;

    const QByteArray mask = currentScopeMask();
    const fill::Plan plan = fill::planFor(mask, m_doc.info());
    if (!plan.isValid()) {
        showError(tr("Nothing to fill"), tr("No MCUs are in scope."));
        return;
    }

    ReferenceFillDialog dialog(m_doc.baseBytes(), m_doc.info(), m_doc.rgb(), mask,
                               QFileInfo(m_doc.filePath()).absolutePath(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString name = QFileInfo(dialog.referencePath()).fileName();
    const QPoint offset = dialog.offset();
    QString description = tr("Filled %1 MCU(s) from %2").arg(dialog.mcuCount()).arg(name);
    if (offset != QPoint(0, 0)) {
        description += tr(", read %1 row(s) and %2 column(s) away")
                           .arg(offset.y())
                           .arg(offset.x());
    }
    description += QStringLiteral(".");

    if (!commit({dialog.op()}, description))
        return;

    // The one step in the list that is not purely a coefficient shuffle, so say
    // what it cost where the rest of the session's arithmetic is recorded.
    log(tr("Those MCUs were compressed with this file's own quantization tables; every MCU "
           "outside them keeps its exact coefficients.%1")
            .arg(dialog.wasScaled()
                     ? tr(" The reference was scaled to this file's dimensions on the way in.")
                     : QString()));
}

void MainWindow::updateClipboardInfo()
{
    if (!m_clipboard.isValid()) {
        m_clipboardLabel->setText(tr("Clipboard: empty."));
        return;
    }
    QString text = tr("Clipboard: %1 MCU(s) from row %2, col %3")
                       .arg(m_clipboard.mcuCount)
                       .arg(m_clipboard.srcRow)
                       .arg(m_clipboard.srcCol);
    if (!m_clipboardSource.isEmpty() && m_clipboardSource != m_doc.filePath())
        text += tr(" of %1").arg(QFileInfo(m_clipboardSource).fileName());
    m_clipboardLabel->setText(text + QStringLiteral("."));
}

bool MainWindow::commit(const QVector<jr::Op> &ops, const QString &description)
{
    cancelPreview();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const bool ok = m_doc.addOps(ops, description, &error);
    QApplication::restoreOverrideCursor();

    if (!ok) {
        showError(tr("Repair failed"), error);
        return false;
    }
    m_showingPreview = false;
    refreshFromDocument();
    log(description);
    return true;
}

// ---------------------------------------------------------------------------
// Preview
// ---------------------------------------------------------------------------

void MainWindow::schedulePreview()
{
    if (!m_doc.isOpen())
        return;
    if (!hasPendingEdit() || m_compareAction->isChecked()) {
        cancelPreview();
        showBaseline();
        return;
    }
    m_previewTimer->start();
}

void MainWindow::cancelPreview()
{
    m_previewTimer->stop();
    // Results already in flight are discarded by their generation stamp.
    ++m_previewGeneration;
}

void MainWindow::showBaseline()
{
    if (!m_showingPreview || !m_doc.isOpen())
        return;
    m_grid->setPixmap(QPixmap::fromImage(
        (m_compareAction->isChecked() ? m_doc.originalRgb() : m_doc.rgb()).toImage()));
    m_showingPreview = false;
}

void MainWindow::onPreviewTimeout()
{
    const QVector<jr::Op> ops = pendingOps();
    if (ops.isEmpty()) {
        showBaseline();
        return;
    }

    const quint64 generation = ++m_previewGeneration;
    // Shared, immutable snapshots: the worker never reads state the UI thread
    // may be editing, and nothing is copied until the worker writes.
    const std::shared_ptr<const jr::Coefs> state = m_doc.coefs();
    const jr::Samples rgb = m_doc.rgb();

    m_previewWatcher->setFuture(QtConcurrent::run([generation, state, rgb, ops]() {
        PreviewResult result;
        result.generation = generation;
        // Only the MCU rows the pending edit changes are decoded.
        auto preview = ImageDocument::preview(state, rgb, ops, &result.error);
        if (!preview)
            return result;
        result.rgb = preview->rgb;
        result.ok = true;
        return result;
    }));
}

void MainWindow::onPreviewReady()
{
    if (m_previewWatcher->isCanceled())
        return;
    const PreviewResult result = m_previewWatcher->result();
    if (result.generation != m_previewGeneration)
        return; // superseded while it was rendering

    if (!result.ok) {
        log(tr("Preview failed: %1").arg(result.error));
        return;
    }
    m_grid->setPixmap(QPixmap::fromImage(result.rgb.toImage()));
    m_showingPreview = true;
}

// ---------------------------------------------------------------------------
// View refresh
// ---------------------------------------------------------------------------

void MainWindow::loadIntoView()
{
    m_compareAction->setChecked(false);
    m_pendingMcuShift = m_pendingUnitShift = 0;
    m_shiftLabel->hide();
    m_damage = analysis::DamageMap{};
    m_grid->setImage(QPixmap::fromImage(m_doc.rgb().toImage()), m_doc.info());
    m_unitCombo->clear();
    for (int u = 0; u < m_doc.info().blocksPerMcu; ++u)
        m_unitCombo->addItem(m_doc.info().unitName(u));
    // What syncGridGeometry compares against to notice a byte edit that
    // changed the frame.
    m_unitCombo->setProperty("mcusX", m_doc.info().mcusX);
    m_unitCombo->setProperty("mcusY", m_doc.info().mcusY);
    // The view pads this out into the scrollable scene rect so the image can
    // be dragged freely rather than sitting locked in the middle.
    m_view->setContentRect(m_grid->boundingRect());
    m_view->zoomToFit();
    updateWindowTitle();
    refreshStepList();
    updateActionStates();
    updateImageInfo();
    updateSelectionInfo();
    updateMatchInfo();
    updateClipboardInfo();
    refreshAnalysis();
    refreshOverlay();
}

void MainWindow::syncGridGeometry()
{
    // A byte edit can change the frame header, and with it the grid.
    const jr::Info &now = m_doc.info();
    if (now.mcusX != m_unitCombo->property("mcusX").toInt()
        || now.mcusY != m_unitCombo->property("mcusY").toInt()
        || now.blocksPerMcu != m_unitCombo->count()) {
        m_grid->setImage(QPixmap::fromImage(m_doc.rgb().toImage()), now);
        m_view->setContentRect(m_grid->boundingRect());
        m_unitCombo->clear();
        for (int u = 0; u < now.blocksPerMcu; ++u)
            m_unitCombo->addItem(now.unitName(u));
    }
    m_unitCombo->setProperty("mcusX", now.mcusX);
    m_unitCombo->setProperty("mcusY", now.mcusY);
}

void MainWindow::refreshAnalysis()
{
    m_analysisDock->setDocument(&m_doc);
    m_damage = analysis::DamageMap{};
}

void MainWindow::refreshOverlay()
{
    if (!m_doc.isOpen() || m_overlay == Overlay::None) {
        m_grid->setOverlay(QImage());
        return;
    }
    const jr::Info &info = m_doc.info();
    QImage img(info.mcusX, info.mcusY, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    if (m_overlay == Overlay::Damage) {
        if (!m_damage.isValid()) {
            const QVector<qint32> src = m_doc.unitSources();
            m_damage = analysis::damage(info, m_doc.ycbcr(), &m_analysisDock->map(), &src);
        }
        for (int m = 0; m < m_damage.score.size(); ++m) {
            const float s = m_damage.score[m];
            if (s > 0.2f)
                img.setPixelColor(m % info.mcusX, m / info.mcusX, QColor(255, 40, 40, int(40 + 140 * s)));
        }
    } else {
        const QByteArray flags = m_doc.provenance();
        for (int m = 0; m < flags.size(); ++m) {
            const quint8 f = quint8(flags[m]);
            QColor c;
            if (f & ImageDocument::kNoData)
                c = QColor(220, 30, 30, 150);
            else if (f & JR_TRACE_PASTED)
                c = QColor(220, 40, 220, 130);
            else if (f & JR_TRACE_MOVED)
                c = QColor(40, 120, 255, 90);
            else if (f & JR_TRACE_DC)
                c = QColor(250, 210, 40, 90);
            else if (f & ImageDocument::kReencoded)
                c = QColor(120, 120, 120, 60);
            else
                continue;
            img.setPixelColor(m % info.mcusX, m / info.mcusX, c);
        }
    }
    m_grid->setOverlay(img);
}

void MainWindow::refreshFromDocument()
{
    syncGridGeometry();
    m_grid->setPixmap(QPixmap::fromImage(
        (m_compareAction->isChecked() ? m_doc.originalRgb() : m_doc.rgb()).toImage()));
    // Picked blocks were measured against the previous state, so their numbers
    // no longer describe what is on screen. A reference measured in another
    // image is untouched by anything that happens here, and keeping it is the
    // point of having it: correcting a whole image against one thumbnail patch
    // takes several rounds of match-and-apply, and re-picking it each time
    // would mean re-finding the same spot each time.
    clearPickedBlocks(true);

    // Every path that changes the recipe ends up here, which makes this the one
    // place the session has to be written down. It costs a couple of kilobytes
    // of JSON against a render that has already happened.
    saveProject();

    updateWindowTitle();
    refreshStepList();
    updateActionStates();
    updateImageInfo();
    updateSelectionInfo();
    updateMatchInfo();
    updateClipboardInfo();
    refreshAnalysis();
    refreshOverlay();
}

void MainWindow::updateWindowTitle()
{
    if (!m_doc.isOpen()) {
        setWindowTitle(tr("MCU Studio"));
        return;
    }
    // The asterisk no longer means unsaved -- nothing here is ever unsaved --
    // but not yet exported: there are repairs the picture on disk does not have.
    setWindowTitle(tr("%1%2 - MCU Studio")
                       .arg(m_doc.fileName(), m_doc.hasUnexportedChanges()
                                                  ? QStringLiteral("*")
                                                  : QString()));
}

void MainWindow::updateActionStates()
{
    const bool open = m_doc.isOpen();
    const bool selection = open && m_grid->hasSelection();

    m_exportAction->setEnabled(open);
    m_exportAsAction->setEnabled(open);
    m_undoAction->setEnabled(m_doc.canUndo());
    m_redoAction->setEnabled(m_doc.canRedo());
    m_resetAction->setEnabled(open && m_doc.stepCount() > 0);

    const int stepRow = m_stepList->currentRow();
    const bool haveStep = open && stepRow >= 0 && stepRow < m_doc.stepCount();
    m_stepList->setEnabled(open);
    m_stepRemoveButton->setEnabled(haveStep);
    m_stepUpButton->setEnabled(haveStep && stepRow > 0);
    m_stepDownButton->setEnabled(haveStep && stepRow + 1 < m_doc.stepCount());
    m_selectAllAction->setEnabled(open);
    m_clearSelectionAction->setEnabled(selection);
    m_copySelectionAction->setEnabled(selection);
    const bool canPaste = selection && m_clipboard.fits(m_doc.info());
    m_pasteOverAction->setEnabled(canPaste);
    m_pasteInsertAction->setEnabled(canPaste);
    // The whole-image scope needs no selection, and is how a picture with
    // nothing left to select from gets filled.
    m_fillReferenceAction->setEnabled(
        open && (selection || scopeChoice() == ScopeChoice::WholeImage));

    m_scopeCombo->setEnabled(open);
    for (int c = 0; c < 3; ++c) {
        m_sliders[c]->setEnabled(open);
        m_spins[c]->setEnabled(open);
    }
    m_applyColorButton->setEnabled(hasPendingColorEdit());
    m_reportAction->setEnabled(true);
    m_resetDeltasButton->setEnabled(open);
    m_pickReferenceButton->setEnabled(open);
    m_pickTargetButton->setEnabled(open);
    m_referenceFromImageButton->setEnabled(open);
    m_matchButton->setEnabled(m_referenceStats.has_value() && m_targetStats.has_value());
    m_blockCountSpin->setEnabled(open);
    m_unitCombo->setEnabled(open);
    m_unitCountSpin->setEnabled(open);
    m_compareAction->setEnabled(open);
    m_nextDamageAction->setEnabled(open);
    if (QStandardItemModel *model = qobject_cast<QStandardItemModel *>(m_scopeCombo->model())) {
        if (QStandardItem *item = model->item(int(ScopeChoice::SelectionToRestart)))
            item->setEnabled(open && m_doc.info().restartInterval > 0);
    }
    m_copyRowSpin->setEnabled(open);
    m_copyColSpin->setEnabled(open);
}

void MainWindow::updateImageInfo()
{
    if (!m_doc.isOpen()) {
        m_infoLabel->setText(tr("No image loaded."));
        return;
    }

    const jr::Info &info = m_doc.info();
    QStringList lines;
    lines << tr("%1 × %2 px").arg(info.width).arg(info.height);
    lines << tr("MCU %1 × %2 px  ·  grid %3 × %4  (%5 MCUs, %6 blocks each)")
                 .arg(info.mcuWidth)
                 .arg(info.mcuHeight)
                 .arg(info.mcusX)
                 .arg(info.mcusY)
                 .arg(info.mcuCount())
                 .arg(info.blocksPerMcu);
    lines << tr("Chroma %1  ·  %2  ·  %3")
                 .arg(info.samplingName())
                 .arg(info.progressive ? tr("progressive, %n scan(s)", nullptr, info.scanCount)
                      : info.scanCount > 1 ? tr("sequential, %n scan(s)", nullptr, info.scanCount)
                                           : tr("baseline"))
                 .arg(info.restartInterval > 0 ? tr("restart every %1 MCUs").arg(info.restartInterval)
                                               : tr("no restart markers"));
    lines << tr("DC quant  Y %1  ·  Cb %2  ·  Cr %3")
                 .arg(info.dcQuant[0])
                 .arg(info.dcQuant[1])
                 .arg(info.dcQuant[2]);
    // Everything above describes the donor's header when there is one, so say
    // whose numbers these are.
    if (!m_doc.donorPath().isEmpty())
        lines << tr("Header borrowed from %1").arg(QFileInfo(m_doc.donorPath()).fileName());
    if (m_doc.trailer().present())
        lines << tr("After the image: %1").arg(m_doc.trailer().describe());
    // The grid shows the picture as stored, which is what MCUs are laid out
    // in; viewers rotate it by this tag.
    static const char *const rotations[] = {"", "", QT_TR_NOOP("mirrored"), QT_TR_NOOP("rotated 180°"),
                                            QT_TR_NOOP("mirrored vertically"), QT_TR_NOOP("mirrored and rotated 90°"),
                                            QT_TR_NOOP("rotated 90° clockwise"), QT_TR_NOOP("mirrored and rotated 90°"),
                                            QT_TR_NOOP("rotated 90° counter-clockwise")};
    const int orientation = exif::orientation(m_doc.baseBytes());
    if (orientation > 1)
        lines << tr("Shown as stored; viewers display it %1").arg(tr(rotations[orientation]));
    if (!info.interleavedSingleScan())
        lines << tr("Insert and delete assume MCU order, which this file's scans do not follow.");
    m_infoLabel->setText(lines.join(QStringLiteral("\n")));
    updateDeltaLabels();
}

void MainWindow::updateSelectionInfo()
{
    if (!m_doc.isOpen() || !m_grid->hasSelection()) {
        m_selectionLabel->setText(tr("No MCUs selected."));
        m_selectionColorLabel->clear();
        return;
    }

    int firstRow, firstCol, lastRow, lastCol;
    m_grid->anchorBlock(&firstRow, &firstCol);
    m_grid->lastBlock(&lastRow, &lastCol);
    const int count = m_grid->selectedCount();

    m_selectionLabel->setText(tr("%1 MCU(s), from row %2 col %3 to row %4 col %5.")
                                  .arg(count)
                                  .arg(firstRow)
                                  .arg(firstCol)
                                  .arg(lastRow)
                                  .arg(lastCol));

    if (count > kMaxMeasuredMcus) {
        m_selectionColorLabel->setText(tr("Selection too large to measure."));
        return;
    }

    // Accumulate per-row so a wrapping scan-order range is measured exactly,
    // not as its bounding box.
    double sum[3] = {0, 0, 0};
    qint64 samples = 0;
    const jr::Samples &ycbcr = m_doc.ycbcr();
    for (const QRect &rect : m_grid->selectionRects()) {
        const colormath::BlockStats stats = colormath::measure(ycbcr, rect);
        if (!stats.isValid())
            continue;
        for (int c = 0; c < 3; ++c)
            sum[c] += stats.mean[c] * double(stats.sampleCount);
        samples += stats.sampleCount;
    }

    if (samples == 0) {
        m_selectionColorLabel->clear();
        return;
    }
    const double mean[3] = {sum[0] / samples, sum[1] / samples, sum[2] / samples};
    m_selectionColorLabel->setText(tr("Mean  %1").arg(formatTriple(mean)));
}

void MainWindow::updateDeltaLabels()
{
    const jr::Info &info = m_doc.info();
    for (int c = 0; c < 3; ++c) {
        if (!m_doc.isOpen()) {
            m_shiftLabels[c]->clear();
            continue;
        }
        const double shift = colormath::pixelShiftForCdelta(info.dcQuant[c], deltaFor(c));
        m_shiftLabels[c]->setText(tr("%1 px").arg(shift, 0, 'f', 1));
    }
}

QString MainWindow::describeReferenceSource() const
{
    if (m_reference.kind == Reference::Kind::External) {
        return tr("%1, %2 × %3 px at (%4, %5)")
            .arg(QFileInfo(m_reference.path).fileName())
            .arg(m_reference.rect.width())
            .arg(m_reference.rect.height())
            .arg(m_reference.rect.x())
            .arg(m_reference.rect.y());
    }
    return tr("row %1, col %2").arg(m_reference.row).arg(m_reference.col);
}

void MainWindow::clearPickedBlocks(bool keepSurvivingReference)
{
    if (!keepSurvivingReference || !m_reference.survivesEdit()) {
        m_referenceStats.reset();
        m_reference = Reference();
        m_grid->setMarkedBlock(McuGridItem::PickMode::Reference, -1, -1);
    }
    m_targetStats.reset();
    m_targetRow = m_targetCol = -1;
    m_grid->setMarkedBlock(McuGridItem::PickMode::Target, -1, -1);
}

void MainWindow::updateMatchInfo()
{
    const auto describe = [](const std::optional<colormath::BlockStats> &stats,
                             const QString &where, const QString &name) {
        if (!stats)
            return tr("%1: not picked.").arg(name);
        return tr("%1: %2, %3").arg(name, where, formatTriple(stats->mean));
    };

    m_referenceLabel->setText(
        describe(m_referenceStats, describeReferenceSource(), tr("Reference")));
    m_targetLabel->setText(describe(m_targetStats,
                                    tr("row %1, col %2").arg(m_targetRow).arg(m_targetCol),
                                    tr("Target")));

    QStringList warnings;
    if (m_referenceStats && m_targetStats) {
        // Clipped samples were pinned at the edge of the range by the decoder,
        // so their real values are unknown and only known to lie further out.
        // A mean over them is pulled inward, and the delta derived from it
        // undershoots.
        for (const auto &pair : {std::pair{tr("reference"), &m_referenceStats},
                                 std::pair{tr("target"), &m_targetStats}}) {
            QStringList channels;
            for (int c = 0; c < 3; ++c) {
                const double fraction = (*pair.second)->clipped[c];
                if (fraction > kClippedSampleWarning) {
                    channels << QStringLiteral("%1 %2%")
                                    .arg(QString::fromLatin1(colormath::channelNames[c]))
                                    .arg(fraction * 100.0, 0, 'f', 0);
                }
            }
            if (!channels.isEmpty()) {
                warnings << tr("Samples clipped in the %1 (%2): the match will fall short.")
                                .arg(pair.first, channels.join(QStringLiteral(", ")));
            }
        }

        // Cb and Cr came back as a flat 128 because the reference has no chroma
        // to give, not because the picture is neutral there. Matching all three
        // channels against it would drain the color it is meant to restore.
        if (m_reference.monochrome) {
            warnings << tr("The reference image is grayscale, so its Cb and Cr are neutral 128 "
                           "whatever it shows. Match Y and leave the Cb and Cr deltas at zero, "
                           "or the color will be pulled out of the image.");
        }

        double delta[3];
        bool large = false;
        for (int c = 0; c < 3; ++c) {
            delta[c] = m_referenceStats->mean[c] - m_targetStats->mean[c];
            if (std::abs(delta[c]) > kLargePixelDelta)
                large = true;
        }
        if (large) {
            warnings << tr("Reference and target differ a lot (Δ %1). That is normal for DC "
                           "damage, but it can also mean they do not show the same content, in "
                           "which case this will overcorrect.")
                            .arg(formatTriple(delta));
        }
    }

    m_matchWarningLabel->setText(warnings.join(QStringLiteral("\n\n")));
    m_matchWarningLabel->setVisible(!warnings.isEmpty());
    updateActionStates();
}

// ---------------------------------------------------------------------------
// Analysis and tools
// ---------------------------------------------------------------------------

void MainWindow::selectMcu(int index, bool center)
{
    const jr::Info &info = m_doc.info();
    if (index < 0 || index >= info.mcuCount())
        return;
    const int row = index / info.mcusX, col = index % info.mcusX;
    m_grid->selectRange(row, col, row, col);
    if (center)
        m_view->centerOn(QRectF(col * info.mcuWidth, row * info.mcuHeight, info.mcuWidth,
                                info.mcuHeight)
                             .center());
}

void MainWindow::onBaseMcuRequested(int baseIndex)
{
    if (!m_doc.isOpen())
        return;
    // The map numbers MCUs as the file holds them; find where that one sits
    // after the recipe's moves.
    const QVector<qint32> src = m_doc.unitSources();
    const int bpm = m_doc.info().blocksPerMcu;
    int at = baseIndex;
    for (int m = 0; m < m_doc.info().mcuCount(); ++m) {
        if (src.value(qsizetype(m) * bpm, -1) == baseIndex * bpm) {
            at = m;
            break;
        }
    }
    selectMcu(at, true);
}

void MainWindow::onNextDamage()
{
    if (!requireImage())
        return;
    if (!m_damage.isValid()) {
        const QVector<qint32> src = m_doc.unitSources();
        m_damage = analysis::damage(m_doc.info(), m_doc.ycbcr(), &m_analysisDock->map(), &src);
    }
    int row = 0, col = 0;
    const int from = m_grid->anchorBlock(&row, &col) ? row * m_doc.info().mcusX + col : -1;
    int next = analysis::nextDamaged(m_damage, from);
    if (next < 0 && from >= 0)
        next = analysis::nextDamaged(m_damage, -1); // wrap around
    if (next < 0) {
        log(tr("No MCU looks damaged."));
        return;
    }
    selectMcu(next, true);
    log(tr("MCU %1 (row %2, col %3) looks damaged.")
            .arg(next)
            .arg(next / m_doc.info().mcusX)
            .arg(next % m_doc.info().mcusX));
}

void MainWindow::onAutoAlign()
{
    if (!requireImage() || !requireSelection())
        return;
    int row = 0, col = 0;
    m_grid->anchorBlock(&row, &col);
    const jr::Info &info = m_doc.info();
    if (row == 0) {
        QMessageBox::information(this, tr("Find the alignment"),
                                 tr("The alignment is judged against the MCU row above the "
                                    "selection, so select an MCU below the first row."));
        return;
    }
    const int start = row * info.mcusX + col;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QVector<analysis::AlignCandidate> found =
        analysis::autoAlign(info, m_doc.ycbcr(), start, qMin(info.mcuCount(), 3 * info.mcusX));
    QApplication::restoreOverrideCursor();
    if (found.isEmpty()) {
        log(tr("Nothing to align against here."));
        return;
    }

    QStringList items;
    double baseline = 0;
    for (const analysis::AlignCandidate &c : found)
        if (c.shift == 0)
            baseline = c.cost;
    for (int i = 0; i < found.size() && i < 15; ++i) {
        const analysis::AlignCandidate &c = found[i];
        const QString what = c.shift > 0 ? tr("insert %n MCU(s)", nullptr, c.shift)
            : c.shift < 0                ? tr("delete %n MCU(s)", nullptr, -c.shift)
                                         : tr("leave as is");
        items << tr("%1   ·   seam %2%3   ·   step Y %4 Cb %5 Cr %6")
                     .arg(what)
                     .arg(c.cost, 0, 'f', 1)
                     .arg(baseline > 0 ? tr(" (%1% of now)").arg(int(100 * c.cost / baseline)) : QString())
                     .arg(c.offset[0], 0, 'f', 0)
                     .arg(c.offset[1], 0, 'f', 0)
                     .arg(c.offset[2], 0, 'f', 0);
    }
    bool ok = false;
    const QString pick = QInputDialog::getItem(
        this, tr("Find the alignment"),
        tr("Shifts of the stream at row %1, col %2, ranked by how well the content below continues "
           "the content above (with any constant color step taken out, since a DC drift is "
           "corrected separately). Pick one to preview it; Ctrl+Enter commits.")
            .arg(row)
            .arg(col),
        items, 0, false, &ok);
    if (!ok)
        return;
    const int i = int(items.indexOf(pick));
    m_pendingMcuShift = 0;
    m_pendingUnitShift = 0;
    nudgeShift(found[i].shift, 0);
}

void MainWindow::onAutoDc()
{
    if (!requireImage())
        return;
    if (scopeChoice() != ScopeChoice::WholeImage && !requireSelection())
        return;
    const QByteArray mask = currentScopeMask();
    const analysis::DcEstimate est = analysis::autoDc(*m_doc.coefs(), mask);
    if (!est.valid) {
        QMessageBox::information(this, tr("Estimate DC offset"),
                                 tr("The scope has no edge against MCUs outside it to measure. "
                                    "Choose a scope that starts where the damage starts."));
        return;
    }
    m_updatingControls = true;
    for (int c = 0; c < 3; ++c) {
        const int delta = qBound(-colormath::kCdeltaLimit, est.cdelta[c], colormath::kCdeltaLimit);
        m_sliders[c]->setValue(delta);
        m_spins[c]->setValue(delta);
    }
    m_updatingControls = false;
    updateDeltaLabels();
    updateActionStates();
    log(tr("Estimated from %1 boundary pixels: Y %2, Cb %3, Cr %4 (%5 confidence).")
            .arg(est.boundaryPixels)
            .arg(est.cdelta[0])
            .arg(est.cdelta[1])
            .arg(est.cdelta[2])
            .arg(est.confidence >= 0.7 ? tr("high") : est.confidence >= 0.4 ? tr("moderate") : tr("low")));
    schedulePreview();
}

void MainWindow::onByteEdits(const QVector<ByteEdit> &edits, const QString &description)
{
    if (!requireImage())
        return;
    cancelPreview();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const bool ok = m_doc.addByteEdits(edits, description, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        showError(tr("Could not edit the stream"), error);
        return;
    }
    m_showingPreview = false;
    refreshFromDocument();
    log(description);
}

void MainWindow::onEmbeddedImages()
{
    if (!requireImage())
        return;
    QByteArray bytes;
    if (!readFile(m_doc.filePath(), &bytes))
        return;
    EmbeddedImagesDialog dialog(bytes, m_doc.filePath(), this);
    if (dialog.isEmpty()) {
        QMessageBox::information(this, tr("Pictures inside this file"),
                                 tr("This file carries no other complete JPEG."));
        return;
    }
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (dialog.chosenUse() == EmbeddedImagesDialog::Use::ColorReference) {
        const jr::Info &info = m_doc.info();
        ReferenceColorDialog ref(dialog.chosenPath(), QSize(info.width, info.height),
                                 m_targetStats ? colormath::mcuRect(info, m_targetRow, m_targetCol) : QRect(),
                                 QString(), this);
        if (ref.exec() != QDialog::Accepted)
            return;
        m_referenceStats = ref.stats();
        m_reference = Reference{Reference::Kind::External, -1, -1, ref.referencePath(), ref.patch(),
                                ref.monochrome()};
        m_grid->setMarkedBlock(McuGridItem::PickMode::Reference, -1, -1);
        updateMatchInfo();
        log(tr("Reference color taken from the embedded picture: %1.")
                .arg(formatTriple(m_referenceStats->mean)));
    } else if (dialog.chosenUse() == EmbeddedImagesDialog::Use::FillReference) {
        log(tr("Saved the embedded picture as %1; choose it in the fill dialog.").arg(dialog.chosenPath()));
        onFillFromReference();
    }
}

void MainWindow::onBatchTriage()
{
    const QString folder = QFileDialog::getExistingDirectory(
        this, tr("Folder to triage"), QFileInfo(m_doc.filePath()).absolutePath());
    if (folder.isEmpty())
        return;
    auto *dialog = new BatchDialog(folder, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &BatchDialog::openRequested, this, [this](const QString &path) { openFile(path); });
    dialog->show();
}

void MainWindow::onAbout()
{
    QMessageBox::about(
        this, tr("About MCU Studio"),
        tr("<h3>MCU Studio %1</h3>"
           "<p>Repairs damaged JPEG files by editing their DCT coefficients directly, "
           "with an MCU-level editor.</p>"
           "<p>Licensed under the GNU General Public License v3.0. The repair transform "
           "is derived from <b>jpegrepair</b> (BSD 3-Clause); see "
           "THIRD-PARTY-NOTICES.md for details.</p>"
           "<p><a href=\"https://github.com/TheGameratorT/mcu-studio\">"
           "github.com/TheGameratorT/mcu-studio</a></p>")
            .arg(QApplication::applicationVersion()));
}

void MainWindow::onCarve()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("File to search for JPEGs"),
                                                      QFileInfo(m_doc.filePath()).absolutePath());
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        showError(tr("Could not read"), file.errorString());
        return;
    }
    // Mapped rather than read, so a disk image larger than memory still works.
    const qint64 size = file.size();
    uchar *mapped = file.map(0, size);
    const QByteArray bytes = mapped ? QByteArray::fromRawData(reinterpret_cast<const char *>(mapped), size)
                                    : file.readAll();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QVector<jpegfile::Embedded> found = jpegfile::carve(bytes);
    QApplication::restoreOverrideCursor();
    if (found.isEmpty()) {
        QMessageBox::information(this, tr("Carve JPEGs"), tr("No complete JPEG was found in %1.")
                                                              .arg(QFileInfo(path).fileName()));
        return;
    }
    const QString out = QFileDialog::getExistingDirectory(
        this, tr("Save the %n JPEG(s) found into", nullptr, int(found.size())), QFileInfo(path).absolutePath());
    if (out.isEmpty())
        return;
    int written = 0;
    for (const jpegfile::Embedded &e : found) {
        QSaveFile f(QDir(out).filePath(QStringLiteral("%1_%2_%3x%4.jpg")
                                           .arg(QFileInfo(path).completeBaseName())
                                           .arg(e.offset, 10, 10, QLatin1Char('0'))
                                           .arg(e.width)
                                           .arg(e.height)));
        if (f.open(QIODevice::WriteOnly) && f.write(bytes.constData() + e.offset, e.length) == e.length
            && f.commit())
            ++written;
    }
    log(tr("Carved %1 of %2 JPEG(s) from %3 into %4.")
            .arg(written)
            .arg(found.size())
            .arg(QFileInfo(path).fileName(), out));
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

void MainWindow::log(const QString &message)
{
    m_log->appendPlainText(QStringLiteral("[%1] %2")
                               .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")),
                                    message));
    statusBar()->showMessage(message, 6000);
}

void MainWindow::showError(const QString &title, const QString &message)
{
    log(QStringLiteral("%1: %2").arg(title, message));
    QMessageBox::critical(this, title, message);
}

bool MainWindow::requireImage()
{
    if (m_doc.isOpen())
        return true;
    QMessageBox::information(this, tr("No image"), tr("Open a JPEG first."));
    return false;
}

bool MainWindow::requireSelection()
{
    if (m_grid->hasSelection())
        return true;
    QMessageBox::information(this, tr("No selection"),
                             tr("Select at least one MCU first. Drag across the image."));
    return false;
}