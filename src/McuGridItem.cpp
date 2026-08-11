// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "McuGridItem.h"

#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QStyleOptionGraphicsItem>

#include <algorithm>

namespace {
// Below this on-screen spacing the grid stops describing the image and
// starts covering it, so it is skipped rather than drawn as a gray wash.
constexpr qreal kMinGridSpacingPx = 5.0;

const QColor kSelectionFill(64, 160, 255, 70);
const QColor kSelectionEdge(96, 180, 255, 220);
const QColor kGridLine(255, 255, 255, 40);
const QColor kReferenceMark(80, 220, 120);
const QColor kTargetMark(255, 160, 60);
const QColor kHoverEdge(255, 255, 255, 160);
} // namespace

McuGridItem::McuGridItem(QGraphicsItem *parent)
    : QGraphicsObject(parent)
{
    setAcceptHoverEvents(true);
    setFlag(QGraphicsItem::ItemUsesExtendedStyleOption, true);
}

void McuGridItem::setImage(const QPixmap &pixmap, const jr::Info &info)
{
    prepareGeometryChange();
    m_pixmap = pixmap;
    m_info = info;
    clearSelection();
    m_referenceRow = m_referenceCol = -1;
    m_targetRow = m_targetCol = -1;
    m_hoverRow = m_hoverCol = -1;
    m_pickMode = PickMode::None;
    update();
}

void McuGridItem::setPixmap(const QPixmap &pixmap)
{
    if (pixmap.size() != m_pixmap.size())
        prepareGeometryChange();
    m_pixmap = pixmap;
    update();
}

QRectF McuGridItem::boundingRect() const
{
    return QRectF(QPointF(0, 0), m_pixmap.size());
}

void McuGridItem::setGridVisible(bool visible)
{
    if (m_gridVisible == visible)
        return;
    m_gridVisible = visible;
    update();
}

void McuGridItem::setSelectionVisible(bool visible)
{
    if (m_selectionVisible == visible)
        return;
    m_selectionVisible = visible;
    update();
}

int McuGridItem::selectedCount() const
{
    if (!hasSelection())
        return 0;
    return std::abs(m_selEnd - m_selStart) + 1;
}

void McuGridItem::fromIndex(int index, int *row, int *col) const
{
    if (m_info.mcusX <= 0) {
        *row = *col = 0;
        return;
    }
    *row = index / m_info.mcusX;
    *col = index % m_info.mcusX;
}

bool McuGridItem::anchorBlock(int *row, int *col) const
{
    if (!hasSelection())
        return false;
    fromIndex(std::min(m_selStart, m_selEnd), row, col);
    return true;
}

bool McuGridItem::lastBlock(int *row, int *col) const
{
    if (!hasSelection())
        return false;
    fromIndex(std::max(m_selStart, m_selEnd), row, col);
    return true;
}

QByteArray McuGridItem::selectionMask() const
{
    if (!m_info.isValid())
        return QByteArray();

    QByteArray mask(qsizetype(m_info.mcusY) * m_info.mcusX, '\0');
    if (!hasSelection())
        return mask;

    const int lo = std::min(m_selStart, m_selEnd);
    const int hi = std::max(m_selStart, m_selEnd);
    std::fill(mask.begin() + lo, mask.begin() + hi + 1, '\1');
    return mask;
}

QVector<QRect> McuGridItem::selectionRects() const
{
    QVector<QRect> rects;
    if (!hasSelection() || !m_info.isValid())
        return rects;

    int anchorRow, anchorCol, lastRow, lastCol;
    anchorBlock(&anchorRow, &anchorCol);
    lastBlock(&lastRow, &lastCol);

    const QRect bounds(0, 0, m_info.width, m_info.height);
    for (int row = anchorRow; row <= lastRow; ++row) {
        int firstCol, endCol;
        if (!rowSpan(row, &firstCol, &endCol))
            continue;
        const QRect r(firstCol * m_info.mcuWidth, row * m_info.mcuHeight,
                      (endCol - firstCol + 1) * m_info.mcuWidth, m_info.mcuHeight);
        const QRect clipped = r.intersected(bounds);
        if (!clipped.isEmpty())
            rects.append(clipped);
    }
    return rects;
}

void McuGridItem::selectAll()
{
    if (!m_info.isValid())
        return;
    m_selStart = 0;
    m_selEnd = m_info.mcuCount() - 1;
    update();
    emit selectionChanged();
}

void McuGridItem::clearSelection()
{
    if (!hasSelection())
        return;
    m_selStart = m_selEnd = -1;
    update();
    emit selectionChanged();
}

void McuGridItem::selectRange(int startRow, int startCol, int endRow, int endCol)
{
    if (!m_info.isValid())
        return;
    const int last = m_info.mcuCount() - 1;
    m_selStart = std::clamp(toIndex(startRow, startCol), 0, last);
    m_selEnd = std::clamp(toIndex(endRow, endCol), 0, last);
    update();
    emit selectionChanged();
}

void McuGridItem::setPickMode(PickMode mode)
{
    m_pickMode = mode;
    setCursor(mode == PickMode::None ? Qt::ArrowCursor : Qt::CrossCursor);
}

void McuGridItem::setMarkedBlock(PickMode which, int row, int col)
{
    if (which == PickMode::Reference) {
        m_referenceRow = row;
        m_referenceCol = col;
    } else if (which == PickMode::Target) {
        m_targetRow = row;
        m_targetCol = col;
    }
    update();
}

bool McuGridItem::blockAt(const QPointF &pos, int *row, int *col) const
{
    if (!m_info.isValid())
        return false;
    const int r = int(pos.y()) / m_info.mcuHeight;
    const int c = int(pos.x()) / m_info.mcuWidth;
    if (pos.x() < 0 || pos.y() < 0 || r >= m_info.mcusY || c >= m_info.mcusX)
        return false;
    *row = r;
    *col = c;
    return true;
}

bool McuGridItem::rowSpan(int row, int *firstCol, int *lastCol) const
{
    if (!hasSelection())
        return false;

    const int lo = std::min(m_selStart, m_selEnd);
    const int hi = std::max(m_selStart, m_selEnd);
    int loRow, loCol, hiRow, hiCol;
    fromIndex(lo, &loRow, &loCol);
    fromIndex(hi, &hiRow, &hiCol);

    if (row < loRow || row > hiRow)
        return false;

    *firstCol = (row == loRow) ? loCol : 0;
    *lastCol = (row == hiRow) ? hiCol : m_info.mcusX - 1;
    return *firstCol <= *lastCol;
}

void McuGridItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *)
{
    const QRectF exposed = option->exposedRect.isEmpty() ? boundingRect() : option->exposedRect;

    painter->setRenderHint(QPainter::SmoothPixmapTransform,
                           option->levelOfDetailFromTransform(painter->worldTransform()) < 1.0);
    painter->drawPixmap(exposed, m_pixmap, exposed);

    if (!m_info.isValid())
        return;

    const qreal scale = option->levelOfDetailFromTransform(painter->worldTransform());

    if (m_selectionVisible)
        paintSelection(painter, exposed);
    if (m_gridVisible)
        paintGrid(painter, exposed, scale);

    paintMark(painter, m_referenceRow, m_referenceCol, kReferenceMark);
    paintMark(painter, m_targetRow, m_targetCol, kTargetMark);

    if (m_hoverRow >= 0) {
        painter->setPen(QPen(kHoverEdge, 0));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(QRectF(m_hoverCol * m_info.mcuWidth, m_hoverRow * m_info.mcuHeight,
                                 m_info.mcuWidth, m_info.mcuHeight));
    }
}

void McuGridItem::paintSelection(QPainter *painter, const QRectF &exposed) const
{
    if (!hasSelection())
        return;

    // Only walk the MCU rows the repaint actually covers -- a full-image walk
    // would be tens of thousands of blocks per paint on a large photo.
    const int firstRow = std::max(0, int(exposed.top()) / m_info.mcuHeight);
    const int lastRow = std::min(m_info.mcusY - 1, int(exposed.bottom()) / m_info.mcuHeight);

    painter->setPen(Qt::NoPen);
    painter->setBrush(kSelectionFill);
    for (int row = firstRow; row <= lastRow; ++row) {
        int firstCol, lastCol;
        if (!rowSpan(row, &firstCol, &lastCol))
            continue;
        painter->drawRect(QRectF(firstCol * m_info.mcuWidth, row * m_info.mcuHeight,
                                 (lastCol - firstCol + 1) * m_info.mcuWidth, m_info.mcuHeight));
    }

    // A thin outline on the first and last block makes the range's direction
    // readable when the fill alone is ambiguous.
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(kSelectionEdge, 0));
    int row, col;
    if (anchorBlock(&row, &col))
        painter->drawRect(QRectF(col * m_info.mcuWidth, row * m_info.mcuHeight, m_info.mcuWidth,
                                 m_info.mcuHeight));
    if (lastBlock(&row, &col))
        painter->drawRect(QRectF(col * m_info.mcuWidth, row * m_info.mcuHeight, m_info.mcuWidth,
                                 m_info.mcuHeight));
}

void McuGridItem::paintGrid(QPainter *painter, const QRectF &exposed, qreal scale) const
{
    if (m_info.mcuWidth * scale < kMinGridSpacingPx
        || m_info.mcuHeight * scale < kMinGridSpacingPx)
        return;

    painter->setPen(QPen(kGridLine, 0));

    const int firstCol = std::max(0, int(exposed.left()) / m_info.mcuWidth);
    const int lastCol = std::min(m_info.mcusX, int(exposed.right()) / m_info.mcuWidth + 1);
    const int firstRow = std::max(0, int(exposed.top()) / m_info.mcuHeight);
    const int lastRow = std::min(m_info.mcusY, int(exposed.bottom()) / m_info.mcuHeight + 1);

    for (int c = firstCol; c <= lastCol; ++c) {
        const qreal x = c * m_info.mcuWidth;
        painter->drawLine(QPointF(x, exposed.top()), QPointF(x, exposed.bottom()));
    }
    for (int r = firstRow; r <= lastRow; ++r) {
        const qreal y = r * m_info.mcuHeight;
        painter->drawLine(QPointF(exposed.left(), y), QPointF(exposed.right(), y));
    }
}

void McuGridItem::paintMark(QPainter *painter, int row, int col, const QColor &color) const
{
    if (row < 0 || col < 0)
        return;
    const QRectF rect(col * m_info.mcuWidth, row * m_info.mcuHeight, m_info.mcuWidth,
                      m_info.mcuHeight);
    painter->setBrush(Qt::NoBrush);
    // Two strokes so the marker reads on both light and dark content.
    painter->setPen(QPen(QColor(0, 0, 0, 160), 0));
    painter->drawRect(rect.adjusted(-1, -1, 1, 1));
    painter->setPen(QPen(color, 0));
    painter->drawRect(rect);
}

void McuGridItem::mousePressEvent(QGraphicsSceneMouseEvent *event)
{
    int row, col;
    if (event->button() != Qt::LeftButton || !blockAt(event->pos(), &row, &col)) {
        event->ignore();
        return;
    }

    if (m_pickMode != PickMode::None) {
        const PickMode mode = m_pickMode;
        setPickMode(PickMode::None);
        emit blockPicked(mode, row, col);
        event->accept();
        return;
    }

    const int index = toIndex(row, col);
    if ((event->modifiers() & Qt::ShiftModifier) && hasSelection()) {
        m_selEnd = index; // extend the existing range, keeping its anchor
    } else {
        m_selStart = m_selEnd = index;
    }
    m_dragging = true;
    update();
    emit selectionChanged();
    event->accept();
}

void McuGridItem::mouseMoveEvent(QGraphicsSceneMouseEvent *event)
{
    int row, col;
    if (!m_dragging || !blockAt(event->pos(), &row, &col)) {
        event->ignore();
        return;
    }
    const int index = toIndex(row, col);
    if (index != m_selEnd) {
        m_selEnd = index;
        update();
        emit selectionChanged();
    }
    event->accept();
}

void McuGridItem::mouseReleaseEvent(QGraphicsSceneMouseEvent *event)
{
    m_dragging = false;
    event->accept();
}

void McuGridItem::hoverMoveEvent(QGraphicsSceneHoverEvent *event)
{
    int row, col;
    if (!blockAt(event->pos(), &row, &col)) {
        hoverLeaveEvent(nullptr);
        return;
    }
    if (row == m_hoverRow && col == m_hoverCol)
        return;
    m_hoverRow = row;
    m_hoverCol = col;
    update();
    emit blockHovered(row, col);
}

void McuGridItem::hoverLeaveEvent(QGraphicsSceneHoverEvent *)
{
    if (m_hoverRow < 0)
        return;
    m_hoverRow = m_hoverCol = -1;
    update();
    emit blockHovered(-1, -1);
}