// SPDX-License-Identifier: GPL-3.0-or-later
#include "task_model.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>

TaskModel::TaskModel(QObject *parent) : QAbstractListModel(parent) {}
TaskModel::~TaskModel() {
    read_.reset();
    write_.reset();
    for (auto &task : tasks_)
        zwlr_foreign_toplevel_handle_v1_destroy(task->handle);
    if (manager_)
        zwlr_foreign_toplevel_manager_v1_destroy(manager_);
    if (seat_)
        wl_seat_destroy(seat_);
    if (registry_)
        wl_registry_destroy(registry_);
    if (display_)
        wl_display_disconnect(display_);
}
bool TaskModel::connectDisplay() {
    display_ = wl_display_connect(nullptr);
    if (!display_)
        return false;
    registry_ = wl_display_get_registry(display_);
    static const wl_registry_listener listener{global, globalRemoved};
    wl_registry_add_listener(registry_, &listener, this);
    if (wl_display_roundtrip(display_) < 0 || !manager_ || !seat_)
        return false;
    if (wl_display_roundtrip(display_) < 0)
        return false;
    read_ = std::make_unique<QSocketNotifier>(wl_display_get_fd(display_), QSocketNotifier::Read);
    write_ = std::make_unique<QSocketNotifier>(wl_display_get_fd(display_), QSocketNotifier::Write);
    write_->setEnabled(false);
    connect(read_.get(), &QSocketNotifier::activated, this, [this] {
        if (wl_display_dispatch(display_) < 0) {
            read_->setEnabled(false);
            write_->setEnabled(false);
            Q_EMIT disconnected();
        } else
            flush();
    });
    connect(write_.get(), &QSocketNotifier::activated, this, [this] { flush(); });
    flush();
    return true;
}
void TaskModel::flush() {
    if (!display_)
        return;
    int result = wl_display_flush(display_);
    if (write_)
        write_->setEnabled(result < 0 && errno == EAGAIN);
    if (result < 0 && errno != EAGAIN)
        Q_EMIT disconnected();
}
int TaskModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(tasks_.size());
}
QVariant TaskModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};
    const auto &task = *tasks_[index.row()];
    switch (role) {
    case TaskId:
        return task.id;
    case Title:
        return task.title.isEmpty() ? task.appId : task.title;
    case AppId:
        return task.appId;
    case Active:
        return task.active;
    case Minimized:
        return task.minimized;
    case Maximized:
        return task.maximized;
    default:
        return {};
    }
}
QHash<int, QByteArray> TaskModel::roleNames() const {
    return {{TaskId, "taskId"}, {Title, "title"},         {AppId, "appId"},
            {Active, "active"}, {Minimized, "minimized"}, {Maximized, "maximized"}};
}
TaskModel::Task *TaskModel::find(int id) {
    for (auto &task : tasks_)
        if (task->id == id)
            return task.get();
    return nullptr;
}
void TaskModel::activate(int id) {
    auto *task = find(id);
    if (!task || !seat_)
        return;
    if (task->active && !task->minimized)
        zwlr_foreign_toplevel_handle_v1_set_minimized(task->handle);
    else {
        zwlr_foreign_toplevel_handle_v1_unset_minimized(task->handle);
        zwlr_foreign_toplevel_handle_v1_activate(task->handle, seat_);
    }
    flush();
}
void TaskModel::minimize(int id) {
    if (auto *task = find(id))
        zwlr_foreign_toplevel_handle_v1_set_minimized(task->handle);
    flush();
}
void TaskModel::maximize(int id) {
    if (auto *task = find(id)) {
        if (task->maximized)
            zwlr_foreign_toplevel_handle_v1_unset_maximized(task->handle);
        else
            zwlr_foreign_toplevel_handle_v1_set_maximized(task->handle);
    }
    flush();
}
void TaskModel::close(int id) {
    if (auto *task = find(id))
        zwlr_foreign_toplevel_handle_v1_close(task->handle);
    flush();
}
void TaskModel::showDesktop() {
    for (const auto &task : tasks_)
        zwlr_foreign_toplevel_handle_v1_set_minimized(task->handle);
    flush();
}
void TaskModel::move(int from, int to, int count) {
    int rows = rowCount();
    if (count < 1 || from < 0 || to < 0 || from + count > rows || to + count > rows || from == to)
        return;
    // beginMoveRows wants the row the block lands in front of, counted before the move.
    if (!beginMoveRows({}, from, from + count - 1, {}, to > from ? to + count : to))
        return;
    auto first = tasks_.begin() + from, last = first + count;
    if (to > from)
        std::rotate(first, last, last + (to - from));
    else
        std::rotate(tasks_.begin() + to, first, last);
    endMoveRows();
}
void TaskModel::changed(Task *task) {
    for (int i = 0; i < rowCount(); ++i)
        if (tasks_[i].get() == task) {
            Q_EMIT dataChanged(index(i), index(i));
            return;
        }
}
void TaskModel::removed(Task *task) {
    for (int i = 0; i < rowCount(); ++i)
        if (tasks_[i].get() == task) {
            beginRemoveRows({}, i, i);
            zwlr_foreign_toplevel_handle_v1_destroy(task->handle);
            tasks_.erase(tasks_.begin() + i);
            endRemoveRows();
            return;
        }
}
void TaskModel::global(void *data, wl_registry *registry, uint32_t name, const char *interface,
                       uint32_t version) {
    auto &self = *static_cast<TaskModel *>(data);
    if (!std::strcmp(interface, "zwlr_foreign_toplevel_manager_v1")) {
        self.manager_ = static_cast<zwlr_foreign_toplevel_manager_v1 *>(wl_registry_bind(
            registry, name, &zwlr_foreign_toplevel_manager_v1_interface, std::min(version, 2u)));
        static const zwlr_foreign_toplevel_manager_v1_listener listener{newTask, finished};
        zwlr_foreign_toplevel_manager_v1_add_listener(self.manager_, &listener, &self);
    } else if (!std::strcmp(interface, "wl_seat") && !self.seat_) {
        self.seat_ =
            static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    }
}
void TaskModel::globalRemoved(void *, wl_registry *, uint32_t) {}
void TaskModel::newTask(void *data, zwlr_foreign_toplevel_manager_v1 *,
                        zwlr_foreign_toplevel_handle_v1 *handle) {
    auto &self = *static_cast<TaskModel *>(data);
    auto task = std::make_unique<Task>();
    task->model = &self;
    task->handle = handle;
    task->id = self.nextId_++;
    static const zwlr_foreign_toplevel_handle_v1_listener listener{title, appId, output, output,
                                                                   state, done,  closed, nullptr};
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &listener, task.get());
    int row = self.rowCount();
    self.beginInsertRows({}, row, row);
    self.tasks_.push_back(std::move(task));
    self.endInsertRows();
}
void TaskModel::finished(void *data, zwlr_foreign_toplevel_manager_v1 *) {
    Q_EMIT static_cast<TaskModel *>(data)->disconnected();
}
void TaskModel::title(void *data, zwlr_foreign_toplevel_handle_v1 *, const char *value) {
    static_cast<Task *>(data)->title = QString::fromUtf8(value);
}
void TaskModel::appId(void *data, zwlr_foreign_toplevel_handle_v1 *, const char *value) {
    static_cast<Task *>(data)->appId = QString::fromUtf8(value);
}
void TaskModel::output(void *, zwlr_foreign_toplevel_handle_v1 *, wl_output *) {}
void TaskModel::state(void *data, zwlr_foreign_toplevel_handle_v1 *, wl_array *states) {
    auto &task = *static_cast<Task *>(data);
    task.active = task.minimized = task.maximized = false;
    const auto *values = static_cast<const uint32_t *>(states->data);
    for (size_t i = 0; i < states->size / sizeof(uint32_t); ++i) {
        task.active |= values[i] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED;
        task.minimized |= values[i] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED;
        task.maximized |= values[i] == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED;
    }
}
void TaskModel::done(void *data, zwlr_foreign_toplevel_handle_v1 *) {
    auto *task = static_cast<Task *>(data);
    task->model->changed(task);
}
void TaskModel::closed(void *data, zwlr_foreign_toplevel_handle_v1 *) {
    auto *task = static_cast<Task *>(data);
    task->model->removed(task);
}
