#pragma once
#include "shaode/config.hpp"
#include "task_model.hpp"
#include <QColor>
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
    Q_PROPERTY(QVariantList pinned READ pinned NOTIFY appsChanged)
    Q_PROPERTY(QVariantList apps READ apps NOTIFY appsChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(TaskModel *tasks READ tasks CONSTANT)
  public:
    explicit ShellController(std::filesystem::path path, QObject *parent = nullptr);
    ~ShellController() override;
    QColor accent() const;
    QColor panelColor() const;
    QColor textColor() const;
    QColor background() const;
    QUrl wallpaper() const;
    int panelHeight() const { return config_.shell.panel_height; }
    bool enabled() const { return config_.shell.enabled; }
    QVariantList pinned() const;
    QVariantList apps() const;
    QString error() const { return error_; }
    TaskModel *tasks() { return &tasks_; }
    Q_INVOKABLE bool launch(const QString &id);
    Q_INVOKABLE QVariantList searchApps(const QString &query) const;
    Q_INVOKABLE void refreshApps();
    Q_INVOKABLE void reload();
    Q_INVOKABLE void clearError();
  Q_SIGNALS:
    void configChanged();
    void appsChanged();
    void errorChanged();
    void disabled();

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
    void report(const QString &message);
    void clearApps();
    static QVariantMap record(const App &app);
};
