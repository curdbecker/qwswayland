/*
 * debug.cpp - Qt widget state inspection for coordinate debugging
 * SPDX-License-Identifier: MIT
 */
#include "debug.h"

#include <QApplication>
#include <QWidget>
#include <QList>
#include <QPoint>
#include <QRect>

#include <stdio.h>
#include <signal.h>

/* UTF-8 box-drawing characters, same as pstree uses */
#define CON_TEE    "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 "  /* ├── */
#define CON_CORNER "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 "  /* └── */
#define CON_VERT   "\xe2\x94\x82   "                          /* │   */
#define CON_SPACE  "    "

/* node_pfx: printed before the widget's first line (contains the branch character)
 * body_pfx: printed before each subsequent detail line (aligns with node text) */
static void dump_widget_info(const QWidget *w, int index,
                             const char *node_pfx, const char *body_pfx)
{
    QPoint screen = w->mapToGlobal(QPoint(0, 0));
    QRect  geom   = w->geometry();
    QRect  frame  = w->frameGeometry();
    QByteArray name  = w->objectName().toLocal8Bit();
    QByteArray title = w->windowTitle().toLocal8Bit();

    fprintf(stderr, "%s[%d] %s", node_pfx, index, w->metaObject()->className());
    if (!name.isEmpty())  fprintf(stderr, "  name=\"%s\"",  name.constData());
    if (!title.isEmpty()) fprintf(stderr, "  title=\"%s\"", title.constData());
    fprintf(stderr, "  [%s %s winId=%lu]\n",
        w->isWindow()  ? "toplevel" : "child",
        w->isVisible() ? "visible"  : "hidden",
        (unsigned long)w->winId());

    fprintf(stderr, "%sgeometry: (%d,%d) %dx%d",
        body_pfx, geom.x(), geom.y(), geom.width(), geom.height());
    if (geom != frame)
        fprintf(stderr, "  frame: (%d,%d) %dx%d",
            frame.x(), frame.y(), frame.width(), frame.height());
    fprintf(stderr, "  screen: (%d,%d)\n", screen.x(), screen.y());

    if (w->parentWidget()) {
        QPoint inParent = w->mapToParent(QPoint(0, 0));
        fprintf(stderr, "%sparent: %s   mapToParent(0,0)=(%d,%d)\n",
            body_pfx,
            w->parentWidget()->metaObject()->className(),
            inParent.x(), inParent.y());
    }
}

/* prefix:  the inherited ancestor prefix built up during recursion
 * is_root: true only for top-level windows (no branch connector)
 * is_last: whether this node is the last sibling (selects ├── vs └──) */
static void dump_tree(const QWidget *w, int *index,
                      const char *prefix, bool is_root, bool is_last)
{
    char node_pfx[256];
    char body_pfx[256];
    char next_pfx[256];  /* prefix passed down to children */

    if (is_root) {
        /* Top-level window: no branch character; children indent under it */
        snprintf(node_pfx, sizeof(node_pfx), "%s", prefix);
        snprintf(body_pfx, sizeof(body_pfx), "%s    ", prefix);
        snprintf(next_pfx, sizeof(next_pfx), "%s    ", prefix);
    } else {
        const char *conn = is_last ? CON_CORNER : CON_TEE;
        const char *cont = is_last ? CON_SPACE  : CON_VERT;
        snprintf(node_pfx, sizeof(node_pfx), "%s%s", prefix, conn);
        snprintf(body_pfx, sizeof(body_pfx), "%s%s", prefix, cont);
        snprintf(next_pfx, sizeof(next_pfx), "%s%s", prefix, cont);
    }

    dump_widget_info(w, *index, node_pfx, body_pfx);
    (*index)++;

    QList<QWidget *> kids;
    Q_FOREACH(QObject *child, w->children()) {
        if (QWidget *cw = qobject_cast<QWidget *>(child))
            kids.append(cw);
    }
    for (int i = 0; i < kids.size(); i++)
        dump_tree(kids[i], index, next_pfx, false, i == kids.size() - 1);
}

static void sigusr1_handler(int sig)
{
    (void)sig;
    qd_widgets();
}

void qtdebug_init(void)
{
    signal(SIGUSR1, sigusr1_handler);
}

__attribute__((used, noinline))
void qd_widgets(void)
{
    fprintf(stderr, "\n=== Qt widget dump ===\n");
    int index = 0;
    Q_FOREACH(QWidget *w, QApplication::topLevelWidgets()) {
        if (index > 0) fprintf(stderr, "\n");
        dump_tree(w, &index, "", true, false);
    }
    fprintf(stderr, "======================\n\n");
}

__attribute__((used, noinline))
void qd_winid(int winid)
{
    Q_FOREACH(QWidget *w, QApplication::topLevelWidgets()) {
        if ((int)w->winId() == winid) {
            int index = 0;
            dump_tree(w, &index, "", true, false);
            return;
        }
    }
    fprintf(stderr, "qd: winId %d not found\n", winid);
}
