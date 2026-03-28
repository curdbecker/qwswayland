/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the examples of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:BSD$
** You may use this file under the terms of the BSD license as follows:
**
** "Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**   * Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**   * Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in
**     the documentation and/or other materials provided with the
**     distribution.
**   * Neither the name of The Qt Company Ltd nor the names of its
**     contributors may be used to endorse or promote products derived
**     from this software without specific prior written permission.
**
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
** LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
** OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
**
** $QT_END_LICENSE$
**
****************************************************************************/

//! [0]
#include <cstdlib>
#include <QObject>
#include <QWidget>
#include <QEvent>
#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QMetaEnum>
#include <QDebug>

#include "mainwindow.h"
#include "debug.h"

/// Gives human-readable event type information.
QDebug operator<<(QDebug str, const QEvent * ev) {
   static int eventEnumIndex = QEvent::staticMetaObject
         .indexOfEnumerator("Type");
   if (ev) {
      QString name = QEvent::staticMetaObject
            .enumerator(eventEnumIndex).valueToKey(ev->type());
      if (!name.isEmpty()) str << name; else str << ev->type();
   } else {
      str << (void*)ev;
   }
   return str;
}

class DebugCursorFilter : public QObject
{
public:
    explicit DebugCursorFilter(QObject *parent = 0)
        : QObject(parent)
        , m_lastGlobalPos(-1, -1)
    {
        qApp->installEventFilter(this);

        foreach (QWidget *w, QApplication::allWidgets())
            w->setMouseTracking(true);
    }

protected:
    bool eventFilter(QObject *obj, QEvent *event)
    {
        if (event->type() != QEvent::SockAct && event->type() != QEvent::MouseMove)
            qDebug() << "EventFilter: event=" << event
                << "obj=" << obj << "widget=" << qobject_cast<QWidget *>(obj);

        // if (event->type() == QEvent::ApplicationDeactivate) {
        //     ::exit(0);
        // }

        // Catch newly created widgets and enable tracking on them
        if (event->type() == QEvent::Create) {
            QWidget *w = qobject_cast<QWidget *>(obj);
            if (w)
                w->setMouseTracking(true);
        } 
	    else if (event->type() == QEvent::MouseMove
         || event->type() == QEvent::MouseButtonPress
         || event->type() == QEvent::MouseButtonRelease)
        {
            QMouseEvent *me = static_cast<QMouseEvent *>(event);
            QWidget *w = qobject_cast<QWidget *>(obj);
            m_lastGlobalPos = me->globalPos();

            qDebug("DebugCursor: global=(%d,%d) local=(%d,%d) widget=%s btn=0x%x",
                   m_lastGlobalPos.x(), m_lastGlobalPos.y(),
                   me->pos().x(), me->pos().y(),
                   w ? w->metaObject()->className() : "??",
                   (int)me->buttons());

            if (w)
                w->update();

        } else if (event->type() == QEvent::Paint)
        {
            QWidget *w = qobject_cast<QWidget *>(obj);
            if (w && m_lastGlobalPos.x() >= 0)
            {
                QPoint localPos = w->mapFromGlobal(m_lastGlobalPos);
                QPainter p(w);
                p.setRenderHint(QPainter::Antialiasing);
                p.setPen(QPen(Qt::white, 2));
                p.drawLine(localPos.x()-10, localPos.y(),
                           localPos.x()+10, localPos.y());
                p.drawLine(localPos.x(), localPos.y()-10,
                           localPos.x(), localPos.y()+10);
                p.setPen(QPen(Qt::black, 1));
                p.setBrush(QColor(255, 0, 0));
                p.drawEllipse(localPos, 4, 4);
            
                return true;
            }
        }

        return QObject::eventFilter(obj, event);
    }
private:
    QPoint m_lastGlobalPos;
};

int main(int argc, char *argv[])
{
    Q_INIT_RESOURCE(example);

    QApplication app(argc, argv);
    app.setOrganizationName("Trolltech");
    app.setApplicationName("Application Example");
    new DebugCursorFilter(qApp);
    qtdebug_init();

    MainWindow *mainWin = nullptr;
    if (QApplication::type() != QApplication::GuiServer) {
        mainWin = new MainWindow;
        mainWin->show();
    }
    return app.exec();
}
//! [0]
