// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller.hpp"
#include "view.hpp"
#include <QFile>
#include <QGuiApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>
#include <functional>
#include <iostream>

int main(int argc, char **argv) {
    // A named screen, as compositor outputs are: the workspace indicator is keyed by it.
    QTemporaryDir screens;
    QFile layout(screens.filePath("screens.json"));
    if (!screens.isValid() || !layout.open(QIODevice::WriteOnly) ||
        layout.write(R"({"screens": [{"name": "TEST-1", "x": 0, "y": 0, "width": 1280,
                         "height": 720, "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1}]})") < 0)
        return 1;
    layout.close();
    qputenv("QT_QPA_PLATFORM", ("offscreen:configfile=" + layout.fileName()).toLocal8Bit());
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
    // A stand-in for the compositor's control socket, with this screen as its only output.
    const auto output = app.primaryScreen()->name();
    auto state = [&output](bool tiling, int workspace) {
        return QString("tiling %1\nworkspace %2\noutput %3 %2 1,2\n")
            .arg(tiling ? "on" : "off")
            .arg(workspace)
            .arg(output)
            .toUtf8();
    };
    QLocalServer compositor;
    QLocalSocket *subscriber = nullptr;
    bool toggled = false;
    QStringList switches;
    QObject::connect(&compositor, &QLocalServer::newConnection, [&] {
        auto *client = compositor.nextPendingConnection();
        QObject::connect(client, &QLocalSocket::readyRead, [&, client] {
            if (!client->canReadLine())
                return;
            auto request = client->readLine();
            if (request == "subscribe\n") {
                subscriber = client;
                client->write("ok\n" + state(false, 2));
            } else if (request == "toggle_tiling\n") {
                toggled = true;
                client->write("ok\n");
                client->disconnectFromServer();
                subscriber->write(state(true, 2));
            } else if (request.startsWith("output ")) {
                switches.push_back(QString::fromUtf8(request).trimmed());
                client->write("ok\n");
                client->disconnectFromServer();
                subscriber->write(state(true, request.trimmed().split(' ').last().toInt()));
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
    // The workspace indicator shows this output's state and switches it.
    // Repeater items are found through the item tree rather than as QObject children.
    std::function<QQuickItem *(QQuickItem *, const QString &)> find =
        [&find](QQuickItem *item, const QString &name) -> QQuickItem * {
        if (item->objectName() == name)
            return item;
        for (auto *child : item->childItems())
            if (auto *found = find(child, name))
                return found;
        return nullptr;
    };
    auto workspace = [&](int number) {
        return find(view.rootObject(), QString("workspace%1").arg(number));
    };
    if (!workspace(4) || workspace(5) || !workspace(2)->property("current").toBool() ||
        workspace(1)->property("current").toBool() ||
        !workspace(1)->property("occupied").toBool() ||
        workspace(3)->property("occupied").toBool()) {
        std::cerr << "the workspace indicator does not show the output's workspaces\n";
        return 1;
    }
    auto centre = [&](QQuickItem *item) {
        return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
    };
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, centre(workspace(3)));
    if (!QTest::qWaitFor([&] { return workspace(3)->property("current").toBool(); }) ||
        switches != QStringList{"output " + output + " workspace 3"}) {
        std::cerr << "clicking a workspace did not switch to it\n";
        return 1;
    }
    auto scroll = [&](int delta) {
        const QPoint at = centre(workspace(2));
        QWheelEvent event(at, view.mapToGlobal(at), QPoint(), QPoint(0, delta), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&view, &event);
    };
    scroll(-120); // down: the next workspace
    if (!QTest::qWaitFor([&] { return workspace(4)->property("current").toBool(); })) {
        std::cerr << "scrolling down did not page to the next workspace\n";
        return 1;
    }
    scroll(-120); // already on the last one
    scroll(60);   // half a notch does nothing yet
    scroll(60);
    if (!QTest::qWaitFor([&] { return workspace(3)->property("current").toBool(); }) ||
        switches.size() != 3 || switches.last() != "output " + output + " workspace 3") {
        std::cerr << "scrolling up did not page back one workspace: "
                  << switches.join(", ").toStdString() << '\n';
        return 1;
    }
    std::cout << "Hover/click, launcher keyboard focus, search, command launch, tiling toggle, and "
                 "workspace indicator passed\n";
}
