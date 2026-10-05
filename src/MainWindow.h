// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#pragma once

#include <QFutureWatcher>
#include <QMainWindow>

#include <optional>

#include "Analysis.h"
#include "ColorMath.h"
#include "DonorHeader.h"
#include "ImageDocument.h"
#include "JpegRepair.h"
#include "McuGridItem.h"
#include "ProjectFile.h"

class QAction;
class QCheckBox;
class QComboBox;
class QGraphicsScene;
class QGroupBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;
class McuGraphicsView;
class AnalysisDock;
class QActionGroup;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // Opens an image, or a project file to resume the session it holds. An
    // image with a project beside it goes through the project, so that
    // reopening a repair and resuming it are the same action.
    bool openFile(const QString &path);
    // Opens `path` by borrowing a header from another JPEG. Offered when
    // opening it the ordinary way fails, and available from the File menu for
    // a file that opens but whose header is plainly not its own.
    bool openWithDonorHeader(const QString &path);

protected:
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void onOpen();
    void onOpenWithDonor();
    void onExport();
    void onExportAs();
    void onUndo();
    void onRedo();
    void onResetToOriginal();

    void onStepItemChanged(QListWidgetItem *item);
    void onStepSelectionChanged();
    void onRemoveStep();
    void onMoveStepUp();
    void onMoveStepDown();

    void onSelectionChanged();
    void onBlockHovered(int row, int col);
    void onBlockPicked(McuGridItem::PickMode mode, int row, int col);

    void onDeltasChanged();
    void onScopeChanged();
    void onPickReferenceFromImage();
    void onMatchColors();
    void onApplyColor();
    void onResetDeltas();
    void onAutoColor();

    void onInsertMcus();
    void onDeleteMcus();
    void onInsertUnits();
    void onDeleteUnits();
    void onCopyBlocks();
    void onAutoAlign();
    void onAutoDc();
    void onNextDamage();
    void onEmbeddedImages();
    void onBatchTriage();
    void onCarve();
    void onAbout();
    void onWriteReport();
    void onByteEdits(const QVector<ByteEdit> &edits, const QString &description);
    void onBaseMcuRequested(int baseIndex);

    void onCopySelection();
    void onPasteOver();
    void onPasteInsert();
    void onFillFromReference();

    void onPreviewTimeout();
    void onPreviewReady();

private:
    // The result of rendering a pending edit, computed off the UI thread.
    struct PreviewResult {
        quint64 generation = 0;
        bool ok = false;
        QString error;
        jr::Samples rgb;
    };

    enum class ScopeChoice { SelectedBlocks, SelectionToEnd, SelectionToRestart, WholeImage };
    enum class Overlay { None, Damage, Provenance };

    // Where the reference color was measured.
    //
    // A block of the image under repair is measured against the render on
    // screen, so it stops describing anything the moment a step is committed. A
    // patch of another copy of the picture is measured against a file this tool
    // never touches, so it outlives every edit -- and it is the only reference
    // there is once a donor header has left the picture wrong from its first
    // block, with no good color anywhere inside the file to point at.
    struct Reference {
        enum class Kind { Block, External };

        Kind kind = Kind::Block;
        int row = -1, col = -1; // Kind::Block, in MCUs
        QString path;           // Kind::External
        QRect rect;             // Kind::External, in that picture's pixels
        // Kind::External, and carried this far because it changes what a match
        // means: a grayscale copy has no chroma to lend.
        bool monochrome = false;

        bool survivesEdit() const { return kind == Kind::External; }
    };

    void buildUi();
    QWidget *buildStepPanel();
    QWidget *buildControlPanel();
    QGroupBox *buildImageGroup();
    QGroupBox *buildSelectionGroup();
    QGroupBox *buildColorGroup();
    QGroupBox *buildBlockGroup();
    void buildActions();

    void log(const QString &message);
    void showError(const QString &title, const QString &message);
    bool requireImage();
    bool requireSelection();

    void loadIntoView();
    // Shared tail of every way of opening a file: clears what belonged to the
    // last one, then puts the new document on screen. `startProject` writes the
    // project file at once for a document that is a reconstruction, whose work
    // exists nowhere else; a session being restored from a project passes false,
    // because the file it would write is the one still being read.
    void finishOpen(bool startProject = true);
    // Resumes the session in `projectPath`: reopens the image the way the
    // project says it was opened, then replays the recipe onto it.
    bool openProject(const QString &projectPath);
    // Writes the session out. Called after every change to the recipe, which is
    // what makes this tool have no Save: the work is on disk before the user
    // has had time to wonder whether it is. Failure is reported once per
    // document rather than on every keystroke -- see m_projectBroken.
    void saveProject();
    // Exports to `path`, writing the report beside it when asked to.
    bool doExport(const QString &path);
    // The current session, in the form the project file stores.
    project::Project currentProject() const;
    // Renames a project that could not be read or replayed out of the way, so
    // that the session about to start does not autosave over the record of the
    // one that could not be recovered. Returns where it was put.
    QString setAsideProject(const QString &projectPath);
    // The dialog offered when a file will not open on its own. Returns true if
    // the user went on to open it with a donor header.
    bool offerDonorHeader(const QString &path, const QString &whyItFailed);
    // Says, once and to the user's face, that part of the picture data is not
    // in the file. Both open paths go through here.
    void reportTrimmedScan(const QString &path);
    bool readFile(const QString &path, QByteArray *out);
    bool runDonorDialog(const QString &path, const QByteArray &broken);
    void refreshFromDocument();  // after a commit, undo or redo
    void updateWindowTitle();
    void updateActionStates();
    void updateImageInfo();
    void updateSelectionInfo();
    void updateDeltaLabels();
    void updateMatchInfo();
    // Where the reference came from, for the panel and the log: an MCU address
    // or a file and the patch inside it.
    QString describeReferenceSource() const;
    // Forgets both picked blocks. `keepSurvivingReference` spares a reference
    // that does not depend on the render -- see MainWindow::Reference.
    void clearPickedBlocks(bool keepSurvivingReference);
    void updateClipboardInfo();
    void refreshStepList();
    // Re-renders after the recipe changed under the list's own controls,
    // reporting failure through the log rather than a dialog: toggling a step
    // off can leave a later one pointing past the end of the stream, and that
    // is a thing to notice, not an error to interrupt over.
    void applyStepChange(bool ok, const QString &error, const QString &description);
    // Shared by both paste modes: `insert` opens a hole first instead of
    // writing over what is already there.
    void pasteClipboard(bool insert);

    ScopeChoice scopeChoice() const;
    jr::Scope currentScope() const;
    // The same choice of blocks as currentScope(), spelled out as one byte per
    // MCU. The fill needs to know which blocks it covers before it runs, to
    // work out how much of the reference to compress.
    QByteArray currentScopeMask() const;
    int deltaFor(int component) const;
    QVector<jr::Op> pendingColorOps() const;
    // The live, uncommitted shift the [ and ] keys build up, as ops.
    QVector<jr::Op> pendingShiftOps() const;
    // Everything previewed but not committed: the shift first, then color.
    QVector<jr::Op> pendingOps() const;
    bool hasPendingColorEdit() const;
    bool hasPendingEdit() const;
    void nudgeShift(int mcus, int units);
    void commitShift();
    void cancelShift();
    void selectMcu(int index, bool center);
    void refreshOverlay();
    void refreshAnalysis();
    // The grid over the current render, with the same geometry check the
    // document makes: a byte edit can change the frame.
    void syncGridGeometry();

    void schedulePreview();
    void cancelPreview();
    void showBaseline();

    // Applies ops and commits, keeping the view, history and log in step.
    bool commit(const QVector<jr::Op> &ops, const QString &description);

    ImageDocument m_doc;
    QString m_exportPath;  // empty until the user has chosen where output goes
    QString m_projectPath; // where this session is being written down
    // How the donor header was assembled, for a reconstruction; written to the
    // project so reopening rebuilds the same bytes.
    donor::SpliceOptions m_donorOptions;
    // Set when writing the project failed. The folder holding a damaged file is
    // not always one we can write to -- read-only rescue media, a mounted disk
    // image -- and a tool that quietly stops saving in that case would be worse
    // than one that never saved at all. Said once, and again at closing time.
    bool m_projectBroken = false;

    QGraphicsScene *m_scene = nullptr;
    McuGraphicsView *m_view = nullptr;
    McuGridItem *m_grid = nullptr;
    QPlainTextEdit *m_log = nullptr;

    QAction *m_openAction = nullptr;
    QAction *m_openDonorAction = nullptr;
    QAction *m_exportAction = nullptr;
    QAction *m_exportAsAction = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_resetAction = nullptr;
    QAction *m_zoomFitAction = nullptr;
    QAction *m_zoomActualAction = nullptr;
    QAction *m_zoomInAction = nullptr;
    QAction *m_zoomOutAction = nullptr;
    QAction *m_gridAction = nullptr;
    QAction *m_selectionOverlayAction = nullptr;
    QAction *m_selectAllAction = nullptr;
    QAction *m_clearSelectionAction = nullptr;
    QAction *m_copySelectionAction = nullptr;
    QAction *m_pasteOverAction = nullptr;
    QAction *m_pasteInsertAction = nullptr;
    QAction *m_fillReferenceAction = nullptr;
    QAction *m_compareAction = nullptr;
    QAction *m_nextDamageAction = nullptr;
    QAction *m_reportAction = nullptr;
    QAction *m_thumbnailAction = nullptr;
    QAction *m_trailerAction = nullptr;
    QActionGroup *m_overlayGroup = nullptr;
    Overlay m_overlay = Overlay::None;

    QLabel *m_infoLabel = nullptr;
    QLabel *m_selectionLabel = nullptr;
    QLabel *m_selectionColorLabel = nullptr;

    QComboBox *m_scopeCombo = nullptr;
    QSlider *m_sliders[3] = {nullptr, nullptr, nullptr};
    QSpinBox *m_spins[3] = {nullptr, nullptr, nullptr};
    QLabel *m_shiftLabels[3] = {nullptr, nullptr, nullptr};
    QPushButton *m_applyColorButton = nullptr;
    QPushButton *m_resetDeltasButton = nullptr;

    QPushButton *m_pickReferenceButton = nullptr;
    QPushButton *m_pickTargetButton = nullptr;
    QPushButton *m_referenceFromImageButton = nullptr;
    QLabel *m_referenceLabel = nullptr;
    QLabel *m_targetLabel = nullptr;
    QPushButton *m_matchButton = nullptr;
    QLabel *m_matchWarningLabel = nullptr;

    QSpinBox *m_blockCountSpin = nullptr;
    QComboBox *m_unitCombo = nullptr;
    QSpinBox *m_unitCountSpin = nullptr;
    QLabel *m_shiftLabel = nullptr;
    // The keyboard's live shift: whole MCUs and single blocks, previewed
    // at the selection's anchor until committed or cancelled.
    int m_pendingMcuShift = 0;
    int m_pendingUnitShift = 0;
    AnalysisDock *m_analysisDock = nullptr;
    analysis::DamageMap m_damage;
    int m_hoverIndex = -1;
    QSpinBox *m_copyRowSpin = nullptr;
    QSpinBox *m_copyColSpin = nullptr;

    QLabel *m_clipboardLabel = nullptr;
    jr::Clipboard m_clipboard;
    // Which file the clipboard came from. Pasting across files is allowed but
    // worth a warning: the coefficients carry the source's quantization.
    QString m_clipboardSource;

    QListWidget *m_stepList = nullptr;
    QPushButton *m_stepRemoveButton = nullptr;
    QPushButton *m_stepUpButton = nullptr;
    QPushButton *m_stepDownButton = nullptr;
    QLabel *m_stepHintLabel = nullptr;

    QLabel *m_hoverLabel = nullptr;
    QLabel *m_zoomLabel = nullptr;

    std::optional<colormath::BlockStats> m_referenceStats;
    std::optional<colormath::BlockStats> m_targetStats;
    Reference m_reference;
    int m_targetRow = -1, m_targetCol = -1;

    QTimer *m_previewTimer = nullptr;
    QFutureWatcher<PreviewResult> *m_previewWatcher = nullptr;
    quint64 m_previewGeneration = 0;
    bool m_showingPreview = false;
    bool m_updatingControls = false;
    // Repopulating the step list fires itemChanged for every row; without this
    // the checkbox handler would re-commit each one.
    bool m_updatingSteps = false;
};