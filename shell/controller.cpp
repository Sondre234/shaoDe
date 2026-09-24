#include "controller.hpp"
#include <QDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>
#include <algorithm>
#include <gio/gdesktopappinfo.h>
#include <iostream>

ShellController::ShellController(std::filesystem::path path, QObject *parent)
    : QObject(parent), path_(std::move(path)), config_(shaode::load_config(path_)), tasks_(this) {
    refreshApps();
}
ShellController::~ShellController() { clearApps(); }
QColor ShellController::accent() const {
    return QColor(QString::fromStdString(config_.shell.accent));
}
QColor ShellController::panelColor() const {
    return QColor(QString::fromStdString(config_.shell.panel_color));
}
QColor ShellController::textColor() const {
    return QColor(QString::fromStdString(config_.shell.text_color));
}
QColor ShellController::background() const {
    return QColor::fromRgbF(config_.settings.background[0], config_.settings.background[1],
                            config_.settings.background[2]);
}
QUrl ShellController::wallpaper() const {
    if (config_.shell.wallpaper.empty())
        return {};
    std::filesystem::path file(config_.shell.wallpaper);
    if (file.is_relative())
        file = std::filesystem::absolute(path_).parent_path() / file;
    return QUrl::fromLocalFile(QString::fromStdString(file.string()));
}
void ShellController::clearApps() {
    for (auto &app : apps_)
        if (app.info)
            g_object_unref(app.info);
    apps_.clear();
}
void ShellController::refreshApps() {
    clearApps();
    int index = 0;
    for (const auto &launcher : config_.shell.launchers)
        apps_.push_back({QString("pinned:%1").arg(index++), QString::fromStdString(launcher.name),
                         QString::fromStdString(launcher.icon), launcher.command, nullptr, true});
    GList *list = g_app_info_get_all();
    for (GList *item = list; item; item = item->next) {
        auto *info = G_APP_INFO(item->data);
        if (!g_app_info_should_show(info) || !g_app_info_get_id(info))
            continue;
        QString icon = "application-x-executable";
        GIcon *gicon = g_app_info_get_icon(info);
        if (gicon && G_IS_THEMED_ICON(gicon)) {
            const char *const *names = g_themed_icon_get_names(G_THEMED_ICON(gicon));
            if (names && *names)
                icon = QString::fromUtf8(*names);
        } else if (gicon && G_IS_FILE_ICON(gicon)) {
            char *path = g_file_get_path(g_file_icon_get_file(G_FILE_ICON(gicon)));
            if (path) {
                icon = QString::fromUtf8(path);
                g_free(path);
            }
        }
        apps_.push_back({QString::fromUtf8(g_app_info_get_id(info)),
                         QString::fromUtf8(g_app_info_get_display_name(info)),
                         icon,
                         {},
                         G_APP_INFO(g_object_ref(info)),
                         false});
    }
    g_list_free_full(list, g_object_unref);
    std::stable_sort(apps_.begin(), apps_.end(), [](const App &a, const App &b) {
        if (a.pinned != b.pinned)
            return a.pinned;
        if (a.pinned)
            return false;
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    Q_EMIT appsChanged();
}
QVariantMap ShellController::record(const App &app) {
    return {{"appId", app.id}, {"name", app.name}, {"icon", app.icon}, {"pinned", app.pinned}};
}
QVariantList ShellController::apps() const {
    QVariantList list;
    for (const auto &app : apps_)
        list.push_back(record(app));
    return list;
}
QVariantList ShellController::pinned() const {
    QVariantList list;
    for (const auto &app : apps_)
        if (app.pinned)
            list.push_back(record(app));
    return list;
}
QVariantList ShellController::searchApps(const QString &query) const {
    QVariantList list;
    for (const auto &app : apps_)
        if (app.name.contains(query, Qt::CaseInsensitive) ||
            app.id.contains(query, Qt::CaseInsensitive))
            list.push_back(record(app));
    return list;
}
void ShellController::report(const QString &message) {
    error_ = message;
    Q_EMIT errorChanged();
    QTimer::singleShot(8000, this, [this, message] {
        if (error_ == message)
            clearError();
    });
}
void ShellController::clearError() {
    error_.clear();
    Q_EMIT errorChanged();
}
bool ShellController::launch(const QString &id) {
    auto it =
        std::find_if(apps_.begin(), apps_.end(), [&id](const App &app) { return app.id == id; });
    if (it == apps_.end()) {
        report("This application is no longer available.");
        return false;
    }
    if (!it->command.empty()) {
        QProcess process;
        auto env = QProcessEnvironment::systemEnvironment();
        env.remove("QT_WAYLAND_SHELL_INTEGRATION");
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(QDir::homePath());
        process.setProgram(QString::fromStdString(it->command.front()));
        QStringList arguments;
        for (size_t i = 1; i < it->command.size(); ++i)
            arguments << QString::fromStdString(it->command[i]);
        process.setArguments(arguments);
        if (!process.startDetached()) {
            report("Could not launch " + it->name + ": " + process.errorString());
            return false;
        }
    } else {
        GAppLaunchContext *context = g_app_launch_context_new();
        g_app_launch_context_unsetenv(context, "QT_WAYLAND_SHELL_INTEGRATION");
        GError *error = nullptr;
        bool success = g_app_info_launch(it->info, nullptr, context, &error);
        g_object_unref(context);
        if (!success) {
            report("Could not launch " + it->name + ": " +
                   QString::fromUtf8(error ? error->message : "unknown error"));
            if (error)
                g_error_free(error);
            return false;
        }
    }
    clearError();
    return true;
}
void ShellController::reload() {
    try {
        auto next = shaode::load_config(path_);
        config_ = std::move(next);
        refreshApps();
        Q_EMIT configChanged();
        if (!enabled())
            Q_EMIT disabled();
    } catch (const std::exception &error) {
        std::cerr << "Shell reload rejected: " << error.what() << '\n';
        report("Configuration unchanged: " + QString::fromUtf8(error.what()));
    }
}
