// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller.hpp"
#include "view.hpp"
#include <QFile>
#include <QGuiApplication>
#include <QJSValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlComponent>
#include <QQmlEngine>
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
    // Tiling is per output; the focused one it reports first is always the opposite of this
    // screen's, as if another monitor had focus, so the panel must show its own.
    const auto output = app.primaryScreen()->name();
    auto state = [&output](bool tiling, int workspace) {
        return QString("tiling %1\nworkspace %2\noutput %3 %2 1,2 %4\n")
            .arg(tiling ? "off" : "on")
            .arg(workspace)
            .arg(output)
            .arg(tiling ? "on" : "off")
            .toUtf8();
    };
    QLocalServer compositor;
    QLocalSocket *subscriber = nullptr;
    bool toggled = false;
    int currentWorkspace = 2;
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
            } else if (request == "output " + output.toUtf8() + " toggle_tiling\n") {
                toggled = !toggled;
                client->write("ok\n");
                client->disconnectFromServer();
                subscriber->write(state(toggled, currentWorkspace));
            } else if (request.startsWith("output ")) {
                switches.push_back(QString::fromUtf8(request).trimmed());
                client->write("ok\n");
                client->disconnectFromServer();
                currentWorkspace = request.trimmed().split(' ').last().toInt();
                subscriber->write(state(toggled, currentWorkspace));
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
    auto panelTiling = [&view] { return view.rootObject()->property("tiling").toBool(); };
    auto *tiling = view.rootObject()->findChild<QQuickItem *>("tilingToggle");
    if (!tiling || !QTest::qWaitFor([&] { return controller.tilingAvailable(); }) ||
        panelTiling()) {
        std::cerr << "tiling state did not arrive from the control socket\n";
        return 1;
    }
    const QPoint toggle =
        tiling->mapToScene(QPointF(tiling->width() / 2, tiling->height() / 2)).toPoint();
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, toggle);
    if (!QTest::qWaitFor([&] { return toggled && panelTiling(); })) {
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
    // Context menus: a task's, then the bar's. A stand-in task list replaces the Wayland one.
    auto *tasks = view.rootObject()->findChild<QQuickItem *>("taskList");
    QQmlComponent fakeTasks(view.engine());
    fakeTasks.setData("import QtQml.Models\nListModel { ListElement { taskId: 7; title: 'Fake'; "
                      "appId: 'fake'; active: false; minimized: false } }",
                      QUrl());
    QObject *fakeModel = fakeTasks.create();
    if (!tasks || !fakeModel)
        return 1;
    tasks->setProperty("model", QVariant::fromValue(fakeModel));
    QQuickItem *task = nullptr;
    if (!QTest::qWaitFor([&] {
            QMetaObject::invokeMethod(tasks, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, task),
                                      Q_ARG(int, 0));
            return task != nullptr;
        }))
        return 1;
    auto center = [](QQuickItem *item) {
        return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
    };
    auto *menu = view.rootObject()->findChild<QQuickItem *>("contextMenu");
    // Repeater delegates are visual children only, so walk the item tree.
    std::function<QQuickItem *(QQuickItem *, const QString &)> findMenuItem =
        [&](QQuickItem *parent, const QString &text) -> QQuickItem * {
        for (auto *item : parent->childItems()) {
            if (item->objectName() == "contextMenuItem" && item->property("text") == text)
                return item;
            if (auto *found = findMenuItem(item, text))
                return found;
        }
        return nullptr;
    };
    auto menuItem = [&](const QString &text) { return findMenuItem(menu, text); };
    // The whole menu must lie inside the panel surface, which grows to make room for it.
    auto menuShown = [&] {
        QRectF area = menu->mapRectToScene(QRectF(0, 0, menu->width(), menu->height()));
        return menu->isVisible() && view.height() > controller.panelExtent() &&
               area.top() >= 0 && area.bottom() <= view.height();
    };
    const QPoint entry = center(task);
    // Held past the long-press time, which once swallowed the right click.
    QTest::mousePress(&view, Qt::RightButton, Qt::NoModifier, entry);
    QTest::qWait(1000);
    QTest::mouseRelease(&view, Qt::RightButton, Qt::NoModifier, center(task));
    if (!QTest::qWaitFor([&] {
            return view.rootObject()->property("taskMenuId").toInt() == 7 && menuShown();
        }) ||
        !menuItem("Maximize / restore") || !menuItem("Minimize") || !menuItem("Close window")) {
        std::cerr << "right-clicking a task did not show its menu\n";
        return 1;
    }
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, center(menuItem("Minimize")));
    if (!QTest::qWaitFor([&] { return !view.rootObject()->property("menuOpen").toBool(); }) ||
        !QTest::qWaitFor([&] { return view.height() == controller.panelExtent(); })) {
        std::cerr << "choosing a task menu item did not close the menu\n";
        return 1;
    }
    // Empty bar space, right of the only task, opens the bar menu.
    const QPoint empty = task->mapToScene(QPointF(task->width() + 40, task->height() / 2)).toPoint();
    QTest::mouseClick(&view, Qt::RightButton, Qt::NoModifier, empty);
    if (!QTest::qWaitFor([&] {
            return view.rootObject()->property("barMenuOpen").toBool() && menuShown();
        }) ||
        !menuItem("Turn tiling off") || !menuItem("Applications")) {
        std::cerr << "right-clicking empty bar space did not show the bar menu\n";
        return 1;
    }
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, center(menuItem("Turn tiling off")));
    if (!QTest::qWaitFor([&] { return !toggled && !panelTiling(); }) ||
        view.rootObject()->property("menuOpen").toBool()) {
        std::cerr << "the bar menu did not toggle tiling off\n";
        return 1;
    }
    // Dragging a task along the bar moves it, not the whole list.
    // ListModel.append takes JavaScript arguments, so it is reached through the engine.
    QQmlEngine::setObjectOwnership(fakeModel, QQmlEngine::CppOwnership);
    view.engine()
        ->evaluate("(function(model) { model.append({ taskId: 8, title: 'Second', appId: 'fake', "
                   "active: false, minimized: false }) })")
        .call({view.engine()->newQObject(fakeModel)});
    QQuickItem *second = nullptr;
    if (!QTest::qWaitFor([&] {
            QMetaObject::invokeMethod(tasks, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, second),
                                      Q_ARG(int, 1));
            return second != nullptr;
        }))
        return 1;
    auto firstTaskId = [&] {
        QJSValue row;
        QMetaObject::invokeMethod(fakeModel, "get", Q_RETURN_ARG(QJSValue, row), Q_ARG(int, 0));
        return row.property("taskId").toInt();
    };
    const QPoint from = center(task), to = center(second) + QPoint(second->width() / 4, 0);
    QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, from);
    for (int step = 1; step <= 10; ++step) {
        QTest::mouseMove(&view, from + (to - from) * step / 10);
        QTest::qWait(10);
    }
    QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, to);
    if (!QTest::qWaitFor([&] { return firstTaskId() == 8; }) ||
        tasks->property("contentX").toReal() != 0) {
        std::cerr << "dragging a task did not reorder the task list\n";
        return 1;
    }
    std::cout << "Hover/click, launcher keyboard focus, search, command launch, tiling toggle, and "
                 "workspace indicator, task and bar context menus, and task reordering passed\n";
}
