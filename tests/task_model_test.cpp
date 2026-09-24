// SPDX-License-Identifier: GPL-3.0-or-later
#include "task_model.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QThread>
#include <functional>
#include <iostream>
#include <stdexcept>

// Exercise the model used by QML against an actual compositor and xdg client.
// No fake protocol callbacks: every state change must come back over Wayland.
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QProcess client;
    try {
        if (argc != 2)
            throw std::runtime_error("expected Wayland probe executable");
        TaskModel model;
        if (!model.connectDisplay())
            throw std::runtime_error("cannot connect task model");
        auto wait = [&](const std::function<bool()> &condition, const char *message) {
            QElapsedTimer timer;
            timer.start();
            while (!condition()) {
                QCoreApplication::processEvents();
                if (timer.elapsed() > 2000)
                    throw std::runtime_error(message);
                QThread::msleep(1);
            }
        };
        client.start(QString::fromLocal8Bit(argv[1]), {"--external-control"});
        if (!client.waitForStarted(2000))
            throw std::runtime_error("cannot start probe client");
        auto value = [&](int role) { return model.data(model.index(0), role); };
        wait([&] { return model.rowCount() == 1 && value(TaskModel::Active).toBool(); },
             "task did not appear with active state");
        if (value(TaskModel::Title).toString() != "shaoDe protocol probe" ||
            value(TaskModel::AppId).toString() != "shaode-probe")
            throw std::runtime_error("task metadata incorrect");
        int id = value(TaskModel::TaskId).toInt();
        model.activate(id); // Clicking the active task minimizes it.
        wait([&] { return value(TaskModel::Minimized).toBool(); }, "minimize failed");
        model.activate(id);
        wait(
            [&] {
                return value(TaskModel::Active).toBool() && !value(TaskModel::Minimized).toBool();
            },
            "restore/activate failed");
        model.maximize(id);
        wait([&] { return value(TaskModel::Maximized).toBool(); }, "maximize failed");
        model.maximize(id);
        wait([&] { return !value(TaskModel::Maximized).toBool(); }, "unmaximize failed");
        model.showDesktop();
        wait([&] { return value(TaskModel::Minimized).toBool(); }, "show desktop failed");
        model.activate(id);
        wait([&] { return value(TaskModel::Active).toBool(); }, "second activation failed");
        model.close(id);
        wait([&] { return model.rowCount() == 0; }, "closed task was not removed");
        if ((client.state() != QProcess::NotRunning && !client.waitForFinished(2000)) ||
            client.exitStatus() != QProcess::NormalExit || client.exitCode() != 0)
            throw std::runtime_error("probe did not exit cleanly");
        // Stale IDs must be harmless after a window closes.
        model.activate(id);
        model.close(id);
        std::cout
            << "Task model metadata, minimize, restore, maximize, show desktop, close passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n' << client.readAllStandardError().toStdString();
        if (client.state() != QProcess::NotRunning) {
            client.kill();
            client.waitForFinished(2000);
        }
        return 1;
    }
}
