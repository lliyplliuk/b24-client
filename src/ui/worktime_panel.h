#pragma once

#include "core/backend.h"

#include <QElapsedTimer>
#include <QSet>
#include <QWidget>

class QLabel;
class QModelIndex;
class QPushButton;
class QTableView;
class QTimer;

namespace bx::ui {

class WorktimeSheetModel;

class WorktimePanel final : public QWidget {
public:
    explicit WorktimePanel(Backend *backend, QWidget *parent = nullptr, bool readOnly = false);

    void refresh(bool forceWorkday = true);
    void setPortalOnline(bool online);
    void requestWorkdaySummary();
    bool hasConfirmedWorkday() const { return m_hasWorkday; }
    WorkdayState workdayState() const { return m_workday.state; }
    qint64 elapsedWorkdaySeconds() const;

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void applyWorkday(const WorkdayStatus &status);
    void requestWorkdayStatus(bool allowOffscreen = false);
    void updateControls();
    void updateElapsed();
    void updatePeriod();
    void requestMonth(const QDate &month);
    void changeWorkday(WorkdayAction action);
    void changeWorkdayAt(WorkdayAction action);
    void beginWorkdayChange();
    void showSelectedDay(const QModelIndex &index);
    void chooseExport();
    void resetAccount();
    bool onScreen() const;

    Backend *const m_backend;
    const bool m_readOnly;
    bool m_online;
    bool m_loadingWorkday = false;
    bool m_hasWorkday = false;
    bool m_changing = false;
    quint64 m_accountEpoch = 0;
    WorkdayStatus m_workday;
    WorkdayAction m_primaryAction = WorkdayAction::Start;
    QDate m_month;
    QSet<QDate> m_loadingMonths;
    QDate m_exportMonth;
    QString m_exportPath;
    QElapsedTimer m_elapsedSince;
    QTimer *m_timer = nullptr;
    QTimer *m_statusRefresh = nullptr;
    QLabel *m_workdayStatus = nullptr;
    QLabel *m_workdayDetails = nullptr;
    QLabel *m_elapsed = nullptr;
    QLabel *m_workdayError = nullptr;
    QLabel *m_period = nullptr;
    QLabel *m_sheetNotice = nullptr;
    QLabel *m_selection = nullptr;
    QLabel *m_exportStatus = nullptr;
    QWidget *m_sheetHeading = nullptr;
    QWidget *m_emptySpace = nullptr;
    QPushButton *m_primary = nullptr;
    QPushButton *m_finish = nullptr;
    QPushButton *m_startAt = nullptr;
    QPushButton *m_finishAt = nullptr;
    QPushButton *m_previous = nullptr;
    QPushButton *m_next = nullptr;
    QPushButton *m_export = nullptr;
    QTableView *m_table = nullptr;
    WorktimeSheetModel *m_model = nullptr;
};

} // namespace bx::ui
