// SPDX-License-Identifier: GPL-3.0-only
// Part of MCU Studio. See LICENSE and THIRD-PARTY-NOTICES.md.

// Zoom/pan around the MCU grid. Zooming is anchored under the cursor so the
// block you are looking at stays put, which matters when hunting for a single
// damaged MCU in a 200-block-wide image.
//
// The scene rect is kept padded by roughly a viewport on every side (see
// updateSceneExtent) so the image is never pinned to the middle of the view:
// there is always somewhere to scroll to, which is what makes cursor-anchored
// zoom and free dragging work even when the image is smaller than the widget.
#pragma once

#include <QGraphicsView>
#include <QPoint>
#include <QRectF>

class McuGraphicsView : public QGraphicsView
{
    Q_OBJECT

public:
    explicit McuGraphicsView(QWidget *parent = nullptr);

    // The image's own bounds, in scene coordinates. Drives both zoom-to-fit
    // and the padding around the scrollable area; call it instead of setting
    // the scene rect directly.
    void setContentRect(const QRectF &rect);

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
    // Fraction of a viewport of slack added on each side of the image.
    static constexpr qreal kPanMargin = 0.9;

    void updateSceneExtent();
    void beginPan(const QPoint &viewportPos);
    void endPan();
    void refreshCursor();

    QRectF m_contentRect;
    QPoint m_panAnchor;
    bool m_panning = false;
    bool m_spaceHeld = false;
};