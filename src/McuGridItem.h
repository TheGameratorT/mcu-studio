// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// The image plus its MCU grid and block selection.
//
// Selection works like text selection rather than a rectangle: a range in
// MCU scan order, from an anchor block to a focus block, wrapping across
// rows. That matches how JPEG damage actually runs -- corruption follows the
// entropy-coded stream, not the picture's geometry.
#pragma once

#include <QByteArray>
#include <QGraphicsObject>
#include <QImage>
#include <QPixmap>

#include "JpegRepair.h"

class McuGridItem : public QGraphicsObject
{
    Q_OBJECT

public:
    enum class PickMode { None, Reference, Target };
    Q_ENUM(PickMode)

    explicit McuGridItem(QGraphicsItem *parent = nullptr);

    void setImage(const QPixmap &pixmap, const jr::Info &info);
    void setPixmap(const QPixmap &pixmap); // same geometry, new pixels

    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option,
               QWidget *widget) override;

    bool gridVisible() const { return m_gridVisible; }
    void setGridVisible(bool visible);
    bool selectionVisible() const { return m_selectionVisible; }
    void setSelectionVisible(bool visible);

    bool hasSelection() const { return m_selStart >= 0 && m_selEnd >= 0; }
    int selectedCount() const;
    // First block of the range in scan order, which is where a
    // "from here onward" repair starts.
    bool anchorBlock(int *row, int *col) const;
    bool lastBlock(int *row, int *col) const;
    // One byte per MCU, row-major, non-zero where selected.
    QByteArray selectionMask() const;
    // The selection as pixel rectangles, one per MCU row it covers. Callers
    // measuring color over the selection want these rather than the mask.
    QVector<QRect> selectionRects() const;
    void selectAll();
    void clearSelection();
    void selectRange(int startRow, int startCol, int endRow, int endCol);

    PickMode pickMode() const { return m_pickMode; }
    void setPickMode(PickMode mode);

    void setMarkedBlock(PickMode which, int row, int col); // row < 0 clears

    // A color per MCU laid over the picture: mcusX x mcusY, ARGB, scaled up
    // without smoothing so each MCU is one flat tile. A null image removes it.
    void setOverlay(const QImage &perMcu);
    bool hasOverlay() const { return !m_overlay.isNull(); }

signals:
    void selectionChanged();
    void blockPicked(McuGridItem::PickMode mode, int row, int col);
    void blockHovered(int row, int col); // (-1, -1) on leave

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *event) override;
    void hoverMoveEvent(QGraphicsSceneHoverEvent *event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *event) override;

private:
    bool blockAt(const QPointF &pos, int *row, int *col) const;
    int toIndex(int row, int col) const { return row * m_info.mcusX + col; }
    void fromIndex(int index, int *row, int *col) const;
    // Columns of `row` inside the scan-order range, or an empty span.
    bool rowSpan(int row, int *firstCol, int *lastCol) const;
    void paintSelection(QPainter *painter, const QRectF &exposed) const;
    void paintGrid(QPainter *painter, const QRectF &exposed, qreal scale) const;
    void paintMark(QPainter *painter, int row, int col, const QColor &color) const;

    QPixmap m_pixmap;
    QImage m_overlay;
    jr::Info m_info;

    bool m_gridVisible = true;
    bool m_selectionVisible = true;

    int m_selStart = -1; // scan-order index of the anchor
    int m_selEnd = -1;   // scan-order index of the focus
    bool m_dragging = false;

    PickMode m_pickMode = PickMode::None;
    int m_referenceRow = -1, m_referenceCol = -1;
    int m_targetRow = -1, m_targetCol = -1;

    int m_hoverRow = -1, m_hoverCol = -1;
};