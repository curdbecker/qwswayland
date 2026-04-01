/*
 * debug.h - Qt widget state inspection for coordinate debugging
 * SPDX-License-Identifier: MIT
 *
 * Trigger from VSCode debug console (paused):
 *   -exec call qd_widgets()
 *   -exec call qd_winid(3)
 *   -exec call qd_screen()
 *
 * Trigger from shell without stopping the process:
 *   kill -USR1 $(pgrep example)
 */
#ifndef QT_DEBUG_H
#define QT_DEBUG_H

#include <QWidget>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once after QApplication is constructed. Installs SIGUSR1 handler. */
void qtdebug_init(void);

void dump_widget_info(const QWidget *w, int index,
                      const char *node_pfx, const char *body_pfx);

/* Dump all widgets as a tree with full coordinate info. */
void qd_widgets(void);

/* Dump all screen geometries reported by QDesktopWidget. */
void qd_screen(void);

/* Find and dump a top-level widget by its QWS winId. */
void qd_winid(int winid);

#ifdef __cplusplus
}
#endif

#endif /* QT_DEBUG_H */
