// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

#include "McuGraphicsView.h"

#include <QGraphicsScene>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

McuGraphicsView::McuGraphicsView(QWidget *parent)
    : QGraphicsView(parent)
{
    setRenderHint(QPainter::Antialiasing, false);
    setDragMode(QGraphicsView::NoDrag);
    // The view positions itself (see applyView); Qt's anchors would fight it.
    setTransformationAnchor(QGraphicsView::NoAnchor);
    setResizeAnchor(QGraphicsView::NoAnchor);
    // Hover feedback repaints one MCU at a time, so a partial-update mode is
    // worth having; zoom and preview swaps invalidate everything anyway.
    setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
    // A neutral backdrop so the image's own edges are unambiguous.
    setBackgroundBrush(QColor(24, 25, 27));
    setFrameShape(QFrame::NoFrame);
    // Dragging is the way around; the bars would only mirror m_center.
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

qreal McuGraphicsView::currentScale() const
{
    return m_scale;
}

qreal McuGraphicsView::fitScale() const
{
    if (m_contentRect.isEmpty())
        return 1.0;
    // A small margin so the image's edge is not flush with the widget's.
    const QSizeF avail(std::max(1, viewport()->width() - 4), std::max(1, viewport()->height() - 4));
    return std::min(avail.width() / m_contentRect.width(), avail.height() / m_contentRect.height());
}

void McuGraphicsView::setContentRect(const QRectF &rect)
{
    m_contentRect = rect;
    applyView();
}

// Turns (m_center, m_scale) into the actual transform and scroll position.
// Nothing is clamped: the image can be panned and zoomed anywhere. The scene
// rect is rebuilt around the view each time (the image plus whatever is on
// screen), so the scroll range can never be what holds the view back.
void McuGraphicsView::applyView()
{
    if (!scene() || m_contentRect.isEmpty())
        return;

    if (m_fit) {
        m_scale = fitScale();
        m_center = m_contentRect.center();
    }

    const QSizeF half(viewport()->width() / (2.0 * m_scale), viewport()->height() / (2.0 * m_scale));
    const QRectF visible(m_center - QPointF(half.width(), half.height()), half * 2.0);
    scene()->setSceneRect(m_contentRect.united(visible));

    setTransform(QTransform::fromScale(m_scale, m_scale));
    QGraphicsView::centerOn(m_center);
}

void McuGraphicsView::centerOn(const QPointF &scenePoint)
{
    m_center = scenePoint;
    m_fit = false;
    applyView();
}

void McuGraphicsView::zoomToFit()
{
    m_fit = true;
    applyView();
    emit scaleChanged(currentScale());
}

void McuGraphicsView::zoomToActualSize()
{
    m_fit = false;
    m_scale = 1.0;
    m_center = m_contentRect.center();
    applyView();
    emit scaleChanged(currentScale());
}

void McuGraphicsView::zoomBy(qreal factor)
{
    zoomAt(factor, QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0));
}

// Zooms so the scene point under viewportPos stays under it.
void McuGraphicsView::zoomAt(qreal factor, const QPointF &viewportPos)
{
    const qreal target = std::clamp(m_scale * factor, kMinScale, kMaxScale);
    if (qFuzzyCompare(target, m_scale))
        return;
    const QPointF vpCenter(viewport()->width() / 2.0, viewport()->height() / 2.0);
    const QPointF under = m_center + (viewportPos - vpCenter) / m_scale;
    m_scale = target;
    m_center = under - (viewportPos - vpCenter) / m_scale;
    m_fit = false;
    applyView();
    emit scaleChanged(currentScale());
}

void McuGraphicsView::wheelEvent(QWheelEvent *event)
{
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        event->ignore();
        return;
    }
    // Scale by a fixed ratio per notch rather than by the raw delta, so
    // trackpads and mice with different resolutions feel the same.
    zoomAt(std::pow(1.15, delta / 120.0), event->position());
    event->accept();
}

void McuGraphicsView::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    const qreal before = m_scale;
    applyView();
    if (!qFuzzyCompare(before, m_scale))
        emit scaleChanged(m_scale);
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
        // Move by the cursor's own movement so the scene tracks the hand
        // exactly, at any zoom.
        const QPoint pos = event->position().toPoint();
        const QPoint delta = pos - m_panAnchor;
        m_panAnchor = pos;
        m_center -= QPointF(delta) / m_scale;
        m_fit = false;
        applyView();
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