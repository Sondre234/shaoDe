#include "controller.hpp"
#include "view.hpp"
#include <QFile>
#include <QGuiApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QTest>
#include <iostream>

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    if (argc != 2)
        return 1;
    QTemporaryDir directory;
    if (!directory.isValid())
        return 1;
    auto config = directory.filePath("init.lua");
    auto marker = directory.filePath("launched");
    QFile file(config);
    if (!file.open(QIODevice::WriteOnly))
        return 1;
    // Long Lua strings preserve paths without shell interpolation.
    file.write(
        (QString(
             "return {shell={launchers={{name='Test app',command={[[%1]],'-E','touch',[[%2]]}}}}}")
             .arg(QString::fromLocal8Bit(argv[1]), marker))
            .toUtf8());
    file.close();
    // A stand-in for the compositor's control socket.
    QLocalServer compositor;
    QLocalSocket *subscriber = nullptr;
    bool toggled = false;
    QObject::connect(&compositor, &QLocalServer::newConnection, [&] {
        auto *client = compositor.nextPendingConnection();
        QObject::connect(client, &QLocalSocket::readyRead, [&, client] {
            if (!client->canReadLine())
                return;
            auto request = client->readLine();
            if (request == "subscribe\n") {
                subscriber = client;
                client->write("ok\ntiling off\nworkspace 1\n");
            } else if (request == "toggle_tiling\n") {
                toggled = true;
                client->write("ok\n");
                client->disconnectFromServer();
                subscriber->write("tiling on\nworkspace 1\n");
            }
        });
    });
    if (!compositor.listen(directory.filePath("control.sock")))
        return 1;
    qputenv("SHAODE_SOCKET", compositor.fullServerName().toLocal8Bit());
    ShellController controller(config.toStdString());
    ShellView view(controller, app.primaryScreen(), false, true);
    if (view.status() != QQuickView::Ready)
        return 1;
    view.show();
    if (!QTest::qWaitForWindowExposed(&view))
        return 1;
    const QPoint start(30, controller.panelHeight() / 2);
    QTest::mouseMove(&view, start);
    QTest::qWait(200); // Hover first: a tooltip must not swallow the following press.
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, start);
    if (!QTest::qWaitFor([&] { return view.rootObject()->property("launcherOpen").toBool(); })) {
        std::cerr << "hover then click did not open the launcher\n";
        return 1;
    }
    auto *search = view.rootObject()->findChild<QQuickItem *>("applicationSearch");
    if (!search || !QTest::qWaitFor([&] { return search->hasActiveFocus(); })) {
        std::cerr << "launcher search did not receive keyboard focus\n";
        return 1;
    }
    for (Qt::Key key : {Qt::Key_T, Qt::Key_E, Qt::Key_S, Qt::Key_T})
        QTest::keyClick(&view, key);
    QTest::keyClick(&view, Qt::Key_Return);
    if (!QTest::qWaitFor([&] { return QFile::exists(marker); })) {
        std::cerr << "search and Enter did not launch the configured command\n";
        return 1;
    }
    if (view.rootObject()->property("launcherOpen").toBool()) {
        std::cerr << "launcher remained open after launching\n";
        return 1;
    }
    auto *tiling = view.rootObject()->findChild<QQuickItem *>("tilingToggle");
    if (!tiling || !QTest::qWaitFor([&] { return controller.tilingAvailable(); }) ||
        controller.tiling()) {
        std::cerr << "tiling state did not arrive from the control socket\n";
        return 1;
    }
    const QPoint toggle =
        tiling->mapToScene(QPointF(tiling->width() / 2, tiling->height() / 2)).toPoint();
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, toggle);
    if (!QTest::qWaitFor([&] { return toggled && controller.tiling(); })) {
        std::cerr << "the tiling button did not toggle tiling\n";
        return 1;
    }
    std::cout << "Hover/click, launcher keyboard focus, search, command launch, and tiling toggle "
                 "passed\n";
}
