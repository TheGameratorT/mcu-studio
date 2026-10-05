// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// Zoom/pan around the MCU grid. Zooming is anchored under the cursor so the
// block you are looking at stays put, which matters when hunting for a single
// damaged MCU in a 200-block-wide image.
//
// The view owns its own state (the scene point at the center of the widget, and
// a scale) and applies it as a plain transform; the scroll bars are never used.
// While "fit" is active the scale is recomputed from the widget size, so
// resizing the window keeps the image fitted and centered. Once the user zooms
// or pans, resizing keeps the same scene point at the center instead. There is
// no clamping: the image can be panned and zoomed freely.
#pragma once

#include <QGraphicsView>
#include <QPoint>
#include <QRectF>

class McuGraphicsView : public QGraphicsView
{
    Q_OBJECT

public:
    explicit McuGraphicsView(QWidget *parent = nullptr);

    // The image's own bounds, in scene coordinates. Drives zoom-to-fit; call it
    // instead of setting the scene rect directly.
    void setContentRect(const QRectF &rect);

    // Puts the given scene point at the center of the view. Hides QGraphicsView::centerOn, which would use the scroll bars.
    void centerOn(const QPointF &scenePoint);

    void zoomToFit();
    void zoomToActualSize();
    void zoomBy(qreal factor);
    qreal currentScale() const;

signals:
    void scaleChanged(qreal scale);

protected:
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    static constexpr qreal kMinScale = 0.02;
    static constexpr qreal kMaxScale = 64.0;

    qreal fitScale() const;
    void applyView();
    void zoomAt(qreal factor, const QPointF &viewportPos);
    void beginPan(const QPoint &viewportPos);
    void endPan();
    void refreshCursor();

    QRectF m_contentRect;
    QPointF m_center;       // scene point shown at the center of the widget
    qreal m_scale = 1.0;
    bool m_fit = true;      // scale follows the widget size
    QPoint m_panAnchor;
    bool m_panning = false;
    bool m_spaceHeld = false;
};