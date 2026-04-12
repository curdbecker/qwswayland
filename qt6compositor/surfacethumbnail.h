/*
 * surfacethumbnail.h - Wayland surface thumbnail item for the alt-tab switcher
 * SPDX-License-Identifier: MIT
 */

#ifndef SURFACETHUMBNAIL_H
#define SURFACETHUMBNAIL_H

#include <QObject>
#include <QPainter>
#include <QQuickPaintedItem>
#include <QWaylandSurface>
#include <QWaylandView>

class SurfaceThumbnail : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QWaylandSurface *surface READ surface WRITE setSurface NOTIFY
                   surfaceChanged)

  public:
    SurfaceThumbnail(QQuickItem *parent = nullptr) : QQuickPaintedItem(parent) {

        connect(this, &QQuickItem::visibleChanged, this, [this]() {
            if (isVisible())
                update();
        });
    }

    QWaylandSurface *surface() const { return m_surface; }

    void setSurface(QWaylandSurface *surface) {
        if (m_surface == surface)
            return;
        m_surface = surface;
        emit surfaceChanged();
        update();
    }

    void paint(QPainter *painter) override {
        if (!m_surface || !m_surface->hasContent())
            return;

        auto *view = m_surface->primaryView();
        if (!view)
            return;

        QWaylandBufferRef buf = view->currentBuffer();
        if (buf.isNull()) {
            return;
        }

        QImage img = buf.image();
        if (img.isNull())
            return;

        painter->drawImage(boundingRect(), img, img.rect());
    }

  signals:
    void surfaceChanged();

  private:
    QWaylandSurface *m_surface = nullptr;
};

#endif // SURFACETHUMBNAIL_H
