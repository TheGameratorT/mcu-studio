// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "McuGraphicsView.h"

#include <QGraphicsScene>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

McuGraphicsView::McuGraphicsView(QWidget *parent)
    : QGraphicsView(parent)
{
    setRenderHint(QPainter::Antialiasing, false);
    setDragMode(QGraphicsView::NoDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    // Hover feedback repaints one MCU at a time, so a partial-update mode is
    // worth having; zoom and preview swaps invalidate everything anyway.
    setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
    // A neutral backdrop so the image's own edges are unambiguous.
    setBackgroundBrush(QColor(24, 25, 27));
    setFrameShape(QFrame::NoFrame);
    // The padded scene rect would keep both bars permanently visible and near
    // full length, which says nothing useful; dragging is the way around.
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

qreal McuGraphicsView::currentScale() const
{
    return std::hypot(transform().m11(), transform().m12());
}

void McuGraphicsView::setContentRect(const QRectF &rect)
{
    m_contentRect = rect;
    updateSceneExtent();
}

// Grow the scene rect to the image plus a viewport of slack on each side, so
// there is always scroll range to move into. Panning and cursor-anchored zoom
// both work by scrolling, and a scene rect that hugs the image leaves them
// nothing to do -- that is what pins the image to the center of the view.
void McuGraphicsView::updateSceneExtent()
{
    if (!scene() || m_contentRect.isEmpty())
        return;

    const QRectF visible = mapToScene(viewport()->rect()).boundingRect();
    const QRectF wanted = m_contentRect.adjusted(-visible.width() * kPanMargin,
                                                 -visible.height() * kPanMargin,
                                                 visible.width() * kPanMargin,
                                                 visible.height() * kPanMargin);
    if (wanted == scene()->sceneRect())
        return;

    // Scrollbar values are relative to the scene rect's origin, so moving that
    // origin would shift what is on screen. Pin the current center across the
    // change.
    const QPointF center = mapToScene(viewport()->rect().center());
    scene()->setSceneRect(wanted);
    centerOn(center);
}

void McuGraphicsView::zoomToFit()
{
    if (m_contentRect.isEmpty())
        return;
    fitInView(m_contentRect, Qt::KeepAspectRatio);
    updateSceneExtent();
    emit scaleChanged(currentScale());
}

void McuGraphicsView::zoomToActualSize()
{
    resetTransform();
    updateSceneExtent();
    emit scaleChanged(currentScale());
}

void McuGraphicsView::zoomBy(qreal factor)
{
    const qreal target = std::clamp(currentScale() * factor, kMinScale, kMaxScale);
    const qreal applied = target / currentScale();
    if (qFuzzyCompare(applied, 1.0))
        return;
    scale(applied, applied);
    // The new scale changes how much slack a viewport is worth in scene units.
    updateSceneExtent();
    emit scaleChanged(currentScale());
}

void McuGraphicsView::wheelEvent(QWheelEvent *event)
{
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        QGraphicsView::wheelEvent(event);
        return;
    }
    // Scale by a fixed ratio per notch rather than by the raw delta, so
    // trackpads and mice with different resolutions feel the same.
    zoomBy(std::pow(1.15, delta / 120.0));
    event->accept();
}

void McuGraphicsView::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    // A wider viewport is worth more slack in scene units.
    updateSceneExtent();
}

void McuGraphicsView::beginPan(const QPoint &viewportPos)
{
    m_panning = true;
    m_panAnchor = viewportPos;
    refreshCursor();
}

void McuGraphicsView::endPan()
{
    m_panning = false;
    refreshCursor();
}

void McuGraphicsView::refreshCursor()
{
    if (m_panning)
        setCursor(Qt::ClosedHandCursor);
    else if (m_spaceHeld)
        setCursor(Qt::OpenHandCursor);
    else
        unsetCursor();
}

void McuGraphicsView::mousePressEvent(QMouseEvent *event)
{
    // Middle drag, and space-held or alt-held left drag, pan from anywhere
    // without disturbing the block selection the left button owns.
    const bool panChord = event->button() == Qt::MiddleButton
        || (event->button() == Qt::LeftButton
            && (m_spaceHeld || event->modifiers().testFlag(Qt::AltModifier)));
    if (panChord) {
        beginPan(event->position().toPoint());
        event->accept();
        return;
    }

    // A plain left drag on the backdrop -- anywhere off the image -- pans too,
    // since there is no selection to make out there.
    if (event->button() == Qt::LeftButton && !itemAt(event->position().toPoint())) {
        beginPan(event->position().toPoint());
        event->accept();
        return;
    }

    QGraphicsView::mousePressEvent(event);
}

void McuGraphicsView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning) {
        // Scroll by the cursor's own movement so the scene tracks the hand
        // exactly, at any zoom.
        const QPoint pos = event->position().toPoint();
        const QPoint delta = pos - m_panAnchor;
        m_panAnchor = pos;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void McuGraphicsView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_panning
        && (event->button() == Qt::MiddleButton || event->button() == Qt::LeftButton)) {
        endPan();
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void McuGraphicsView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spaceHeld = true;
        refreshCursor();
        event->accept();
        return;
    }
    QGraphicsView::keyPressEvent(event);
}

void McuGraphicsView::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spaceHeld = false;
        refreshCursor();
        event->accept();
        return;
    }
    QGraphicsView::keyReleaseEvent(event);
}