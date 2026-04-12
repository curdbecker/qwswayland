/*
 * main.cpp - Qt6 Wayland compositor entry point
 * SPDX-License-Identifier: MIT
 */

#include <QtCore/QDebug>
#include <QtCore/QUrl>

#include <QObject>

#include <QtGui/QGuiApplication>

#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>

#include "surfacethumbnail.h"

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);

    qmlRegisterType<SurfaceThumbnail>("WindowSwitcher", 1, 0,
                                      "SurfaceThumbnail");

    QQmlApplicationEngine engine(QUrl("qrc:///qml/main.qml"));

    return app.exec();
}
