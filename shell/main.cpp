#include "controller.hpp"
#include "view.hpp"
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQuickItem>
#include <QScreen>
#include <QSocketNotifier>
#include <QTimer>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

namespace {
volatile sig_atomic_t signalFd = -1;
void onSignal(int number) {
    int previous = errno;
    char byte = number == SIGHUP ? 'r' : 'q';
    if (signalFd >= 0) {
        auto ignored = write(signalFd, &byte, 1);
        (void)ignored;
    }
    errno = previous;
}
} // namespace
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    // Views come and go with outputs (all of them during a VT switch); the shell's lifetime
    // follows the compositor connection instead.
    QGuiApplication::setQuitOnLastWindowClosed(false);
    QCoreApplication::setApplicationName("shaode-shell");
    QGuiApplication::setDesktopFileName("shaode-shell");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"config", "Lua configuration file", "path"});
    parser.addOption({"preview", "Open a normal window for UI development"});
    parser.addOption({"preview-desktop", "Preview the desktop instead of the taskbar"});
    parser.addOption(
        {"quit-after", "Exit after this many milliseconds (for UI tests)", "milliseconds"});
    parser.addOption({"screenshot", "Save a preview screenshot before exiting", "path"});
    parser.process(app);
    if (!parser.isSet("config"))
        parser.showHelp(1);
    const bool preview = parser.isSet("preview");
#if !SHAODE_LAYER_SHELL
    if (!preview) {
        std::cerr
            << "This build supports --preview only; build with LayerShellQt for a desktop shell\n";
        return 1;
    }
#endif
    if (!preview && QGuiApplication::platformName() != "wayland") {
        std::cerr << "shaode-shell requires the Qt Wayland platform\n";
        return 1;
    }
    try {
        ShellController controller(parser.value("config").toStdString());
        if (!controller.enabled())
            return 0;
        QObject::connect(&controller, &ShellController::disabled, &app, &QCoreApplication::quit);
        QObject::connect(controller.tasks(), &TaskModel::disconnected, &app,
                         &QCoreApplication::quit);
        if (!preview && !controller.tasks()->connectDisplay()) {
            std::cerr << "The compositor must support foreign-toplevel-management\n";
            return 1;
        }
        qmlRegisterUncreatableType<TaskModel>("ShaoDe", 1, 0, "TaskModel", "Provided by the shell");
        std::vector<std::unique_ptr<ShellView>> views;
        auto addScreen = [&](QScreen *screen) {
            // Qt's stand-in while the compositor has no outputs has no wl_output to attach to.
            if (!preview && screen->name().isEmpty())
                return;
            for (bool desktop : {true, false}) {
                if (preview && desktop != parser.isSet("preview-desktop"))
                    continue;
                auto view = std::make_unique<ShellView>(controller, screen, desktop, preview);
                if (view->status() == QQuickView::Error) {
                    for (const auto &error : view->errors())
                        std::cerr << error.toString().toStdString() << '\n';
                    throw std::runtime_error("could not load shell QML");
                }
                auto reportFrame = [window = view.get()] {
                    QObject::connect(
                        window, &QQuickWindow::frameSwapped, window,
                        [window] {
                            std::cerr
                                << "shaoDe surface rendered: " << window->title().toStdString()
                                << '\n';
                        },
                        Qt::SingleShotConnection);
                };
                reportFrame();
                QObject::connect(&controller, &ShellController::configChanged, view.get(),
                                 reportFrame);
                view->show();
                if (preview && !desktop)
                    view->rootObject()->setProperty("launcherOpen", true);
                views.push_back(std::move(view));
            }
        };
        for (auto *screen : QGuiApplication::screens()) {
            addScreen(screen);
            if (preview)
                break;
        }
        if (views.empty())
            throw std::runtime_error("no output available");
        QObject::connect(&app, &QGuiApplication::screenAdded, &app, [&](QScreen *screen) {
            if (preview)
                return;
            try {
                addScreen(screen);
            } catch (const std::exception &error) {
                std::cerr << error.what() << '\n';
            }
        });
        QObject::connect(&app, &QGuiApplication::screenRemoved, &app, [&](QScreen *screen) {
            std::erase_if(views,
                          [screen](const auto &view) { return view->outputScreen() == screen; });
        });
        int pipeFds[2];
        if (pipe2(pipeFds, O_NONBLOCK | O_CLOEXEC) < 0)
            throw std::runtime_error("cannot create signal pipe");
        signalFd = pipeFds[1];
        struct sigaction action{};
        action.sa_handler = onSignal;
        sigemptyset(&action.sa_mask);
        for (int signal : {SIGHUP, SIGTERM, SIGINT})
            sigaction(signal, &action, nullptr);
        QSocketNotifier notifier(pipeFds[0], QSocketNotifier::Read);
        QObject::connect(&notifier, &QSocketNotifier::activated, &app, [&] {
            char bytes[64];
            ssize_t count = read(pipeFds[0], bytes, sizeof(bytes));
            for (ssize_t i = 0; i < count; ++i)
                if (bytes[i] == 'r')
                    controller.reload();
                else
                    app.quit();
        });
        if (parser.isSet("quit-after")) {
            bool ok = false;
            int timeout = parser.value("quit-after").toInt(&ok);
            if (!ok || timeout < 1)
                throw std::runtime_error("--quit-after must be a positive integer");
            QTimer::singleShot(timeout, &app, [&] {
                if (parser.isSet("screenshot") &&
                    !views.front()->grabWindow().save(parser.value("screenshot")))
                    app.exit(1);
                else
                    app.quit();
            });
        }
        std::cerr << "shaoDe shell ready: " << views.size() << " surfaces\n";
        int result = app.exec();
        signalFd = -1;
        close(pipeFds[0]);
        close(pipeFds[1]);
        return result;
    } catch (const std::exception &error) {
        std::cerr << "shaode-shell: " << error.what() << '\n';
        return 1;
    }
}
