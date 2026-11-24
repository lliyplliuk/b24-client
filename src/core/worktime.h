#pragma once

#include <QDate>
#include <QString>
#include <QTime>
#include <QVector>

namespace bx {

enum class WorkdayState { Unavailable, Closed, Running, Paused, Expired };
enum class WorkdayAction { Start, Pause, Resume, Finish };

struct WorkdayStatus {
    WorkdayState state = WorkdayState::Unavailable;
    bool canStart = false;
    bool canPause = false;
    bool canResume = false;
    bool canFinish = false;
    bool requiresReport = false;
    qint64 elapsedSeconds = -1;
    QString details;
    bool canStartAt = false;
    bool canFinishAt = false;
    bool requiresTimeReason = true;
    QDate startDate;
    QDate finishDate;
    QTime suggestedFinishTime;
};

struct WorktimeDay {
    QDate date;
    QString value;
    QString details;
};

struct WorktimeEmployee {
    QString name;
    QVector<WorktimeDay> days;
};

struct WorktimeSheet {
    QDate month;
    QVector<WorktimeEmployee> employees;
    bool canExport = false;
    QString notice;
};

} // namespace bx
