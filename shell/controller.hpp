// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "shaode/config.hpp"
#include "task_model.hpp"
#include <QColor>
#include <QLocalSocket>
#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <vector>

typedef struct _GAppInfo GAppInfo;
class ShellController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor accent READ accent NOTIFY configChanged)
    Q_PROPERTY(QColor panelColor READ panelColor NOTIFY configChanged)
    Q_PROPERTY(QColor textColor READ textColor NOTIFY configChanged)
    Q_PROPERTY(QColor background READ background NOTIFY configChanged)
    Q_PROPERTY(QUrl wallpaper READ wallpaper NOTIFY configChanged)
    Q_PROPERTY(int panelHeight READ panelHeight NOTIFY configChanged)
    Q_PROPERTY(bool panelTop READ panelTop NOTIFY configChanged)
    Q_PROPERTY(int panelMarginTop READ panelMarginTop NOTIFY configChanged)
    Q_PROPERTY(int panelMarginRight READ panelMarginRight NOTIFY configChanged)
    Q_PROPERTY(int panelMarginBottom READ panelMarginBottom NOTIFY configChanged)
    Q_PROPERTY(int panelMarginLeft READ panelMarginLeft NOTIFY configChanged)
    Q_PROPERTY(int panelExtent READ panelExtent NOTIFY configChanged)
    Q_PROPERTY(int panelRadius READ panelRadius NOTIFY configChanged)
    Q_PROPERTY(QString fontFamily READ fontFamily NOTIFY configChanged)
    Q_PROPERTY(int fontSize READ fontSize NOTIFY configChanged)
    Q_PROPERTY(QVariantList pinned READ pinned NOTIFY appsChanged)
    Q_PROPERTY(QVariantList apps READ apps NOTIFY appsChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(TaskModel *tasks READ tasks CONSTANT)
    Q_PROPERTY(bool tiling READ tiling NOTIFY tilingChanged)
    Q_PROPERTY(bool tilingAvailable READ tilingAvailable NOTIFY tilingChanged)
    Q_PROPERTY(int workspaceCount READ workspaceCount NOTIFY configChanged)
    Q_PROPERTY(QVariantMap workspaces READ workspaces NOTIFY workspacesChanged)
  public:
    explicit ShellController(std::filesystem::path path, QObject *parent = nullptr);
    ~ShellController() override;
    QColor accent() const;
    QColor panelColor() const;
    QColor textColor() const;
    QColor background() const;
    QUrl wallpaper() const;
    int panelHeight() const { return config_.shell.panel_height; }
    bool panelTop() const { return config_.shell.panel_top; }
    int panelMarginTop() const { return config_.shell.panel_margin[0]; }
    int panelMarginRight() const { return config_.shell.panel_margin[1]; }
    int panelMarginBottom() const { return config_.shell.panel_margin[2]; }
    int panelMarginLeft() const { return config_.shell.panel_margin[3]; }
    // The strip the panel reserves: the bar and the margins above and below it.
    int panelExtent() const { return panelHeight() + panelMarginTop() + panelMarginBottom(); }
    int panelRadius() const { return config_.shell.panel_radius; }
    QString fontFamily() const { return QString::fromStdString(config_.shell.font); }
    int fontSize() const { return config_.shell.font_size; }
    bool enabled() const { return config_.shell.enabled; }
    QVariantList pinned() const;
    QVariantList apps() const;
    QString error() const { return error_; }
    TaskModel *tasks() { return &tasks_; }
    bool tiling() const { return tiling_; }
    bool tilingAvailable() const { return subscribed_; }
    int workspaceCount() const { return config_.settings.workspaces; }
    // By output name: {current: N, occupied: [N, ...], tiling: bool}, numbered from 1.
    QVariantMap workspaces() const { return workspaces_; }
    Q_INVOKABLE bool launch(const QString &id);
    Q_INVOKABLE void refreshApps();
    Q_INVOKABLE void reload();
    Q_INVOKABLE void clearError();
    // Toggles tiling on `output`, or on the focused output when it is empty.
    Q_INVOKABLE void toggleTiling(const QString &output = {});
    Q_INVOKABLE void showWorkspace(const QString &output, int number);
  Q_SIGNALS:
    void configChanged();
    void appsChanged();
    void errorChanged();
    void disabled();
    void tilingChanged();
    void workspacesChanged();
    void launcherRequested(const QString &output);

  private:
    struct App {
        QString id, name, icon;
        shaode::Command command;
        GAppInfo *info = nullptr;
        bool pinned = false;
    };
    std::filesystem::path path_;
    shaode::Config config_;
    TaskModel tasks_;
    std::vector<App> apps_;
    QString error_;
    // Compositor state from its control socket ($SHAODE_SOCKET), kept open by "subscribe".
    QLocalSocket *state_ = nullptr;
    bool subscribed_ = false, tiling_ = false;
    QVariantMap workspaces_, nextWorkspaces_;
    void subscribe();
    void request(const QByteArray &line, const QString &unavailable);
    void report(const QString &message);
    void clearApps();
    static QVariantMap record(const App &app);
};
