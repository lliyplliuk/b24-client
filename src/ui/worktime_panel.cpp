#include "ui/worktime_panel.h"

#include "ui/workspace_style.h"
#include "ui/saved_file_offer.h"
#include "ui/task_dialogs.h"

#include <QAbstractTableModel>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QStandardPaths>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace bx::ui {
namespace {

QDate monthStart(const QDate &date)
{
    return {date.year(), date.month(), 1};
}

QString monthKey(const QDate &month)
{
    return month.toString(QStringLiteral("yyyy-MM"));
}

QString dayState(WorkdayState state)
{
    switch (state) {
    case WorkdayState::Closed: return QStringLiteral("Рабочий день закрыт");
    case WorkdayState::Running: return QStringLiteral("Рабочий день идёт");
    case WorkdayState::Paused: return QStringLiteral("Рабочий день на паузе");
    case WorkdayState::Expired: return QStringLiteral("Предыдущий рабочий день не закрыт");
    case WorkdayState::Unavailable: return QStringLiteral("Учёт рабочего времени недоступен");
    }
    return {};
}

QLabel *caption(QWidget *parent, const QString &accessibleName)
{
    auto *label = workspace::label({}, "quiet", parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setAccessibleName(accessibleName);
    return label;
}

} // namespace

class WorktimeSheetModel final : public QAbstractTableModel {
public:
    using QAbstractTableModel::QAbstractTableModel;

    void setSheet(const WorktimeSheet &sheet)
    {
        beginResetModel();
        m_sheet = sheet;
        endResetModel();
    }

    const WorktimeSheet &sheet() const { return m_sheet; }

    int rowCount(const QModelIndex &parent) const override
    {
        return parent.isValid() ? 0 : m_sheet.employees.size();
    }

    int columnCount(const QModelIndex &parent) const override
    {
        return parent.isValid() ? 0 : m_sheet.month.daysInMonth();
    }

    const WorktimeDay *day(const QModelIndex &index) const
    {
        if (!index.isValid())
            return nullptr;
        const auto date = m_sheet.month.addDays(index.column());
        const auto &days = m_sheet.employees.at(index.row()).days;
        const auto match = std::find_if(days.cbegin(), days.cend(), [&date](const WorktimeDay &day) {
            return day.date == date;
        });
        return match == days.cend() ? nullptr : &*match;
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid())
            return {};
        if (role == Qt::TextAlignmentRole)
            return int(Qt::AlignCenter);
        if (role == Qt::BackgroundRole && m_sheet.month.addDays(index.column()).dayOfWeek() >= Qt::Saturday)
            return QColor("#edf1ed");
        const auto *entry = day(index);
        if (role == Qt::DisplayRole)
            return entry && !entry->value.isEmpty() ? entry->value : QStringLiteral("—");
        if (role == Qt::ToolTipRole && entry && !entry->details.isEmpty())
            return QStringLiteral("<qt>%1</qt>").arg(entry->details.toHtmlEscaped());
        return {};
    }

    QVariant headerData(int index, Qt::Orientation axis, int role) const override
    {
        if (axis == Qt::Vertical) {
            if (index >= 0 && index < m_sheet.employees.size()) {
                const auto &name = m_sheet.employees.at(index).name;
                if (role == Qt::DisplayRole)
                    return name;
                if (role == Qt::ToolTipRole)
                    return QStringLiteral("<qt>%1</qt>").arg(name.toHtmlEscaped());
            }
            return {};
        }
        if (role == Qt::DisplayRole)
            return index + 1;
        if (role == Qt::ToolTipRole)
            return QLocale(QLocale::Russian).toString(m_sheet.month.addDays(index), QStringLiteral("dddd, d MMMM"));
        return {};
    }

private:
    WorktimeSheet m_sheet;
};

WorktimePanel::WorktimePanel(Backend *backend, QWidget *parent, bool readOnly)
    : QWidget(parent), m_backend(backend), m_readOnly(readOnly), m_online(backend->isDemo()),
      m_month(monthStart(QDate::currentDate()))
{
    setObjectName(QStringLiteral("worktimePanel"));
    setAccessibleName(QStringLiteral("Рабочее время"));
    auto *content = new QVBoxLayout(this);
    content->setContentsMargins(24, 18, 24, 16);
    content->setSpacing(10);
    content->addWidget(workspace::label(QStringLiteral("Рабочее время"), "pageTitle", this));

    auto *day = new QHBoxLayout;
    m_workdayStatus = workspace::label({}, "sectionTitle", this);
    m_workdayStatus->setAccessibleName(QStringLiteral("Статус рабочего дня"));
    m_workdayStatus->setWordWrap(true);
    day->addWidget(m_workdayStatus, 1);
    m_elapsed = workspace::label({}, "sectionTitle", this);
    m_elapsed->setAccessibleName(QStringLiteral("Отработано"));
    day->addWidget(m_elapsed);
    m_primary = button({}, QStringLiteral("Действие с рабочим днём"), this);
    m_primary->setObjectName(QStringLiteral("primary"));
    connect(m_primary, &QPushButton::clicked, this, [this] { changeWorkday(m_primaryAction); });
    day->addWidget(m_primary);
    m_finish = button(QStringLiteral("Завершить"), QStringLiteral("Завершить рабочий день"), this);
    connect(m_finish, &QPushButton::clicked, this, [this] { changeWorkday(WorkdayAction::Finish); });
    day->addWidget(m_finish);
    content->addLayout(day);

    auto *alternate = new QHBoxLayout;
    alternate->addStretch(1);
    const auto manualButton = [this, alternate](const QString &title, WorkdayAction action) {
        auto *control = button(title, title, this);
        alternate->addWidget(control);
        connect(control, &QPushButton::clicked, this, [this, action] { changeWorkdayAt(action); });
        return control;
    };
    m_startAt = manualButton(QStringLiteral("Начать с другим временем"), WorkdayAction::Start);
    m_finishAt = manualButton(QStringLiteral("Завершить с другим временем"), WorkdayAction::Finish);
    content->addLayout(alternate);

    m_workdayDetails = caption(this, QStringLiteral("Подробности рабочего дня"));
    m_workdayError = caption(this, QStringLiteral("Ошибка рабочего дня"));
    content->addWidget(m_workdayDetails);
    content->addWidget(m_workdayError);
    content->addSpacing(5);

    m_sheetHeading = new QWidget(this);
    auto *sheet = new QHBoxLayout(m_sheetHeading);
    sheet->setContentsMargins(0, 0, 0, 0);
    sheet->addWidget(workspace::label(QStringLiteral("Табель"), "sectionTitle", this), 1);
    m_previous = button(QStringLiteral("‹"), QStringLiteral("Предыдущий месяц"), this);
    m_next = button(QStringLiteral("›"), QStringLiteral("Следующий месяц"), this);
    m_period = workspace::label({}, "quiet", this);
    m_period->setAccessibleName(QStringLiteral("Месяц табеля"));
    m_period->setAlignment(Qt::AlignCenter);
    m_period->setMinimumWidth(116);
    m_export = button(QStringLiteral("Скачать Excel"), QStringLiteral("Скачать табель Excel"), this);
    sheet->addWidget(m_previous);
    sheet->addWidget(m_period);
    sheet->addWidget(m_next);
    sheet->addSpacing(9);
    sheet->addWidget(m_export);
    content->addWidget(m_sheetHeading);

    m_sheetNotice = caption(this, QStringLiteral("Состояние табеля"));
    content->addWidget(m_sheetNotice);
    m_model = new WorktimeSheetModel(this);
    m_table = new QTableView(this);
    m_table->setAccessibleName(QStringLiteral("Табель по сотрудникам и дням"));
    m_table->setModel(m_model);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setStyleSheet(QStringLiteral(R"(
        QTableView { background: #fffefa; alternate-background-color: #f7f8f3;
                     color: #182d3b; border: 1px solid #dce4e4; gridline-color: #e3e9e6;
                     selection-background-color: #d0e9e4; selection-color: #182d3b; }
        QHeaderView { background: #f0f4ef; }
        QHeaderView::section { color: #214852; background: #e9f0ed; padding: 3px 7px;
                               border: 0; border-bottom: 1px solid #dce4e4; }
        QTableCornerButton::section { background: #e9f0ed; border: 0; }
    )"));
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_table->horizontalHeader()->setDefaultSectionSize(48);
    m_table->verticalHeader()->setDefaultSectionSize(32);
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_table->verticalHeader()->setFixedWidth(156);
    m_table->verticalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    content->addWidget(m_table, 1);
    m_selection = caption(this, QStringLiteral("Подробности выбранного дня"));
    m_selection->setTextInteractionFlags(Qt::TextSelectableByMouse);
    content->addWidget(m_selection);
    m_exportStatus = caption(this, QStringLiteral("Состояние выгрузки табеля"));
    content->addWidget(m_exportStatus);
    m_exportStatus->hide();
    m_emptySpace = new QWidget(this);
    content->addWidget(m_emptySpace, 1);

    m_timer = new QTimer(this);
    m_timer->setObjectName(QStringLiteral("worktimeElapsedTimer"));
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &WorktimePanel::updateElapsed);
    m_statusRefresh = new QTimer(this);
    m_statusRefresh->setObjectName(QStringLiteral("worktimeStatusRefreshTimer"));
    m_statusRefresh->setInterval(60000);
    m_statusRefresh->setTimerType(Qt::VeryCoarseTimer);
    connect(m_statusRefresh, &QTimer::timeout, this, [this] { requestWorkdayStatus(); });
    connect(m_previous, &QPushButton::clicked, this, [this] { requestMonth(m_month.addMonths(-1)); });
    connect(m_next, &QPushButton::clicked, this, [this] { requestMonth(m_month.addMonths(1)); });
    connect(m_export, &QPushButton::clicked, this, &WorktimePanel::chooseExport);
    connect(m_table->selectionModel(), &QItemSelectionModel::currentChanged, this, &WorktimePanel::showSelectedDay);

    connect(m_backend, &Backend::connectionChanged, this, &WorktimePanel::setPortalOnline);
    connect(m_backend, &Backend::accountScopeChanged, this, &WorktimePanel::resetAccount);
    connect(m_backend, &Backend::workdayLoaded, this, [this](const WorkdayStatus &status) {
        if (m_loadingWorkday && !m_changing)
            applyWorkday(status);
    });
    connect(m_backend, &Backend::workdayChanged, this, [this](const WorkdayStatus &status) {
        if (m_changing)
            applyWorkday(status);
    });
    connect(m_backend, &Backend::worktimeSheetLoaded, this, [this](const WorktimeSheet &loaded) {
        if (!m_loadingMonths.remove(loaded.month) || loaded.month != m_month)
            return;
        m_model->setSheet(loaded);
        const auto notice = loaded.notice.isEmpty() && loaded.employees.isEmpty()
            ? QStringLiteral("За выбранный месяц записей нет.") : loaded.notice;
        setStatusLabel(m_sheetNotice, m_online ? notice : QStringLiteral("Нет связи с порталом."));
        m_table->setVisible(!loaded.employees.isEmpty());
        showSelectedDay({});
        updateControls();
    });
    connect(m_backend, &Backend::worktimeSheetExported, this, [this](const QDate &month, const QString &path) {
        if (month != m_exportMonth || path != m_exportPath)
            return;
        m_exportMonth = {};
        m_exportPath.clear();
        setStatusLabel(m_exportStatus, QStringLiteral("Табель сохранён в %1").arg(QDir::toNativeSeparators(path)));
        updateControls();
        offerToOpenSavedFile(this, path);
    });
    connect(m_backend, &Backend::failed, this, [this](const QString &operation, const QString &reason) {
        if (operation == QLatin1String("workday") && m_loadingWorkday) {
            m_loadingWorkday = false;
            setStatusLabel(m_workdayError, reason);
        } else if (operation == QLatin1String("workday-change") && m_changing) {
            m_changing = false;
            setStatusLabel(m_workdayError, reason);
        } else if (operation.startsWith(QLatin1String("worktime-sheet:"))) {
            const auto month = QDate::fromString(operation.mid(15) + QStringLiteral("-01"), Qt::ISODate);
            if (!m_loadingMonths.remove(month) || month != m_month)
                return;
            setStatusLabel(m_sheetNotice, reason);
        } else if (m_exportMonth.isValid() && operation == QStringLiteral("worktime-export:") + monthKey(m_exportMonth)) {
            m_exportMonth = {};
            m_exportPath.clear();
            setStatusLabel(m_exportStatus, reason);
        } else {
            return;
        }
        updateControls();
    });
    m_table->hide();
    if (!m_online)
        setStatusLabel(m_sheetNotice, QStringLiteral("Нет связи с порталом."));
    updatePeriod();
    updateControls();
    showSelectedDay({});
}

void WorktimePanel::refresh(bool forceWorkday)
{
    if (!m_online)
        return;
    if (forceWorkday || !m_hasWorkday || m_elapsedSince.hasExpired(m_statusRefresh->interval()))
        requestWorkdayStatus();
    requestMonth(m_month);
}

bool WorktimePanel::onScreen() const
{
    return isVisible() && !window()->isMinimized();
}

void WorktimePanel::requestWorkdaySummary()
{
    requestWorkdayStatus(true);
}

void WorktimePanel::requestWorkdayStatus(bool allowOffscreen)
{
    if ((!allowOffscreen && !onScreen()) || !m_online || m_changing || m_loadingWorkday)
        return;
    m_loadingWorkday = true;
    setStatusLabel(m_workdayError, {});
    updateControls();
    m_backend->requestWorkday();
}

void WorktimePanel::setPortalOnline(bool online)
{
    if (m_online == online)
        return;
    m_online = online;
    if (online && onScreen()) {
        refresh();
    } else if (!online) {
        m_loadingWorkday = false;
        m_hasWorkday = false;
        setStatusLabel(m_sheetNotice, QStringLiteral("Нет связи с порталом."));
    }
    updateControls();
}

void WorktimePanel::resetAccount()
{
    ++m_accountEpoch;
    m_changing = false;
    m_loadingWorkday = false;
    m_hasWorkday = false;
    m_loadingMonths.clear();
    m_exportMonth = {};
    m_exportPath.clear();
    m_elapsedSince.invalidate();
    m_workday = {};
    m_model->setSheet({});
    m_table->hide();
    setStatusLabel(m_workdayError, {});
    setStatusLabel(m_exportStatus, {});
    setStatusLabel(m_sheetNotice, m_online ? QString() : QStringLiteral("Нет связи с порталом."));
    showSelectedDay({});
    updateControls();
    if (onScreen())
        refresh();
}

void WorktimePanel::applyWorkday(const WorkdayStatus &status)
{
    m_changing = false;
    m_loadingWorkday = false;
    m_workday = status;
    m_hasWorkday = true;
    m_elapsedSince.restart();
    setStatusLabel(m_workdayError, {});
    updateControls();
}

void WorktimePanel::updateControls()
{
    const bool writable = m_online && !m_readOnly && !m_changing && !m_loadingWorkday;
    const auto state = m_workday.state;
    const bool demoUnavailable = m_backend->isDemo() && m_hasWorkday && state == WorkdayState::Unavailable;
    m_workdayStatus->setText(!m_online ? QStringLiteral("Нет связи с порталом")
        : m_loadingWorkday && !m_hasWorkday ? QStringLiteral("Проверяем рабочий день…") : dayState(state));
    setLoadingIndicator(m_workdayStatus, m_online && m_loadingWorkday && !m_hasWorkday);
    const bool running = state == WorkdayState::Running;
    const bool paused = state == WorkdayState::Paused;
    const bool reopen = state == WorkdayState::Closed && m_workday.canResume && !m_workday.canStart;
    QString startHint;
    if (!m_workday.canStartAt) {
        if (reopen)
            startHint = QStringLiteral("Этот рабочий день уже был начат. «Открыть снова» продолжит его с прежним временем начала.");
        else if (running || paused)
            startHint = QStringLiteral("Рабочий день уже начат. Время начала сохранено.");
    }
    auto details = m_changing ? QStringLiteral("Ждём подтверждения портала…")
        : m_readOnly ? QStringLiteral("Режим чтения") : m_online ? m_workday.details : QString();
    if (demoUnavailable)
        details = QStringLiteral("Учёт времени и табель доступны при подключении к рабочему порталу. В деморежиме здесь нет данных.");
    if (m_online && !m_changing && !m_readOnly && !startHint.isEmpty()) {
        if (!details.isEmpty())
            details += QLatin1Char('\n');
        details += startHint;
    }
    setStatusLabel(m_workdayDetails, details, m_changing && m_online);
    const bool resume = paused || reopen;
    m_primaryAction = running ? WorkdayAction::Pause : resume ? WorkdayAction::Resume : WorkdayAction::Start;
    const auto title = running ? QStringLiteral("Пауза") : paused ? QStringLiteral("Продолжить")
        : reopen ? QStringLiteral("Открыть снова") : QStringLiteral("Начать день");
    m_primary->setText(title);
    m_primary->setAccessibleName(title);
    m_primary->setVisible(state != WorkdayState::Unavailable && state != WorkdayState::Expired);
    m_primary->setEnabled(writable && (running ? m_workday.canPause : resume ? m_workday.canResume : m_workday.canStart));
    m_finish->setVisible(running || paused || state == WorkdayState::Expired);
    m_finish->setEnabled(writable && m_workday.canFinish);
    m_startAt->setVisible(m_workday.canStartAt || !startHint.isEmpty());
    m_startAt->setEnabled(writable && m_workday.canStartAt);
    m_startAt->setAccessibleDescription(startHint);
    m_finishAt->setVisible(m_workday.canFinishAt);
    m_finishAt->setEnabled(writable && m_workday.canFinishAt);
    m_sheetHeading->setVisible(!demoUnavailable);
    m_sheetNotice->setVisible(!demoUnavailable && !m_sheetNotice->text().isEmpty());
    m_emptySpace->setVisible(m_table->isHidden());
    m_previous->setEnabled(m_online && m_month.addMonths(-1).isValid());
    m_next->setEnabled(m_online && m_month.addMonths(1).isValid());
    m_export->setEnabled(m_online && !m_exportMonth.isValid() && !m_loadingMonths.contains(m_month)
                         && m_model->sheet().month == m_month && m_model->sheet().canExport);
    updateElapsed();
    if (onScreen() && m_online && running && m_workday.elapsedSeconds >= 0) {
        if (!m_timer->isActive())
            m_timer->start();
    } else {
        m_timer->stop();
    }
    if (onScreen() && m_online && !demoUnavailable) {
        if (!m_statusRefresh->isActive())
            m_statusRefresh->start();
    } else {
        m_statusRefresh->stop();
    }
}

void WorktimePanel::updateElapsed()
{
    const auto elapsed = elapsedWorkdaySeconds();
    if (elapsed < 0 || !m_online) {
        m_elapsed->hide();
        return;
    }
    const auto text = QStringLiteral("Отработано %1:%2:%3").arg(elapsed / 3600, 2, 10, QLatin1Char('0'))
        .arg((elapsed / 60) % 60, 2, 10, QLatin1Char('0')).arg(elapsed % 60, 2, 10, QLatin1Char('0'));
    setStatusLabel(m_elapsed, text);
}

qint64 WorktimePanel::elapsedWorkdaySeconds() const
{
    const auto base = m_workday.elapsedSeconds;
    if (base < 0 || m_workday.state != WorkdayState::Running || !m_elapsedSince.isValid())
        return base;
    return base + m_elapsedSince.elapsed() / 1000;
}

void WorktimePanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    updateControls();
}

void WorktimePanel::hideEvent(QHideEvent *event)
{
    m_timer->stop();
    m_statusRefresh->stop();
    QWidget::hideEvent(event);
}

void WorktimePanel::updatePeriod()
{
    auto name = QLocale(QLocale::Russian).standaloneMonthName(m_month.month(), QLocale::LongFormat);
    if (!name.isEmpty())
        name[0] = name.at(0).toUpper();
    m_period->setText(QStringLiteral("%1 %2").arg(name, QString::number(m_month.year())));
}

void WorktimePanel::requestMonth(const QDate &month)
{
    if (!month.isValid())
        return;
    if (m_month != month) {
        m_loadingMonths.clear(); // Transport coalesces intermediate periods without emitting a result.
        m_month = month;
        updatePeriod();
        m_model->setSheet({});
        m_table->hide();
        showSelectedDay({});
    }
    setStatusLabel(m_sheetNotice, m_online ? QStringLiteral("Загружаем табель…") : QStringLiteral("Нет связи с порталом."),
                   m_online);
    if (!m_online || m_loadingMonths.contains(month))
        return;
    m_loadingMonths.insert(month);
    updateControls();
    m_backend->requestWorktimeSheet(month);
}

void WorktimePanel::changeWorkday(WorkdayAction action)
{
    if (action == WorkdayAction::Finish ? !m_finish->isEnabled() : !m_primary->isEnabled())
        return;
    const auto epoch = m_accountEpoch;
    QString report;
    if (action == WorkdayAction::Finish && m_workday.requiresReport) {
        bool accepted = false;
        report = QInputDialog::getMultiLineText(this, QStringLiteral("Завершить рабочий день"),
                  QStringLiteral("Кратко опишите сделанное за день"), {}, &accepted).trimmed();
        if (!accepted)
            return;
        if (report.isEmpty()) {
            setStatusLabel(m_workdayError, QStringLiteral("Для завершения рабочего дня нужен отчёт."));
            return;
        }
    }
    if (epoch != m_accountEpoch || !m_online || m_changing || m_loadingWorkday
        || (action == WorkdayAction::Finish && !m_workday.canFinish))
        return;
    beginWorkdayChange();
    m_backend->changeWorkday(action, report);
}

void WorktimePanel::beginWorkdayChange()
{
    m_changing = true;
    setStatusLabel(m_workdayError, {});
    updateControls();
}

void WorktimePanel::changeWorkdayAt(WorkdayAction action)
{
    const bool finish = action == WorkdayAction::Finish;
    const auto *trigger = finish ? m_finishAt : m_startAt;
    if (!trigger->isEnabled() || trigger->isHidden())
        return;
    const auto epoch = m_accountEpoch;
    const auto openedState = m_workday.state;
    const auto recommended = finish ? m_workday.finishDate : m_workday.startDate;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("worktimeManualDialog"));
    dialog.setWindowTitle(trigger->text());
    dialog.setMinimumWidth(365);
    dialog.setStyleSheet(QStringLiteral(R"(
        QDialog#worktimeManualDialog { background: #f1f2ed; }
        QDateTimeEdit, QLineEdit#workspaceEditor, QPlainTextEdit#workspaceEditor {
            color: #182d3b; background: #fffefa; border: 1px solid #c8d5d4;
            border-radius: 6px; padding: 5px 7px;
        }
        QDateTimeEdit:focus, QLineEdit#workspaceEditor:focus, QPlainTextEdit#workspaceEditor:focus { border-color: #215c70; }
    )"));
    auto *layout = new QFormLayout(&dialog);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(9);
    auto *date = new QDateEdit(recommended.isValid() ? recommended : QDate::currentDate(), &dialog);
    date->setDisplayFormat(QStringLiteral("dd.MM.yyyy"));
    date->setCalendarPopup(true);
    date->setAccessibleName(QStringLiteral("Дата"));
    layout->addRow(QStringLiteral("Дата"), date);

    const auto suggested = finish ? m_workday.suggestedFinishTime : QTime();
    QLineEdit *explicitTime = nullptr;
    QTimeEdit *time = nullptr;
    if (finish && m_workday.state == WorkdayState::Expired && !suggested.isValid()) {
        explicitTime = new QLineEdit(&dialog);
        editor(explicitTime);
        explicitTime->setInputMask(QStringLiteral("00:00;_"));
    } else {
        time = new QTimeEdit(suggested.isValid() ? suggested : QTime::currentTime(), &dialog);
        time->setDisplayFormat(QStringLiteral("HH:mm"));
    }
    auto *timeField = explicitTime ? static_cast<QWidget *>(explicitTime) : time;
    timeField->setAccessibleName(QStringLiteral("Время"));
    layout->addRow(QStringLiteral("Время"), timeField);
    const auto selectedTime = [explicitTime, time] {
        return explicitTime ? QTime::fromString(explicitTime->text(), QStringLiteral("HH:mm")) : time->time();
    };

    auto *reason = new QLineEdit(&dialog);
    editor(reason);
    reason->setAccessibleName(QStringLiteral("Причина"));
    reason->setPlaceholderText(m_workday.requiresTimeReason ? QStringLiteral("Почему время отличается")
        : QStringLiteral("Необязательно"));
    layout->addRow(QStringLiteral("Причина"), reason);
    QPlainTextEdit *report = nullptr;
    if (finish && m_workday.requiresReport) {
        report = new QPlainTextEdit(&dialog);
        editor(report);
        report->setAccessibleName(QStringLiteral("Отчёт за рабочий день"));
        report->setPlaceholderText(QStringLiteral("Кратко опишите, что сделали за день"));
        report->setMaximumHeight(90);
        layout->addRow(QStringLiteral("Отчёт"), report);
    }
    const bool requiresReason = m_workday.requiresTimeReason;
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto *submit = buttons->button(QDialogButtonBox::Ok);
    submit->setText(finish ? QStringLiteral("Завершить день") : QStringLiteral("Начать день"));
    submit->setObjectName(QStringLiteral("primary"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    const auto validate = [submit, reason, report, requiresReason, selectedTime] {
        submit->setEnabled(selectedTime().isValid() && (!requiresReason || !reason->text().trimmed().isEmpty())
            && (!report || !report->toPlainText().trimmed().isEmpty()));
    };
    connect(reason, &QLineEdit::textChanged, &dialog, validate);
    if (explicitTime)
        connect(explicitTime, &QLineEdit::textChanged, &dialog, validate);
    if (report)
        connect(report, &QPlainTextEdit::textChanged, &dialog, validate);
    validate();
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addRow(buttons);
    timeField->setFocus();
    if (dialog.exec() != QDialog::Accepted || epoch != m_accountEpoch || openedState != m_workday.state
        || !trigger->isEnabled() || trigger->isHidden())
        return;

    const auto explanation = reason->text().trimmed();
    const auto dailyReport = report ? report->toPlainText().trimmed() : QString();
    if ((m_workday.requiresTimeReason && explanation.isEmpty()) || (finish && m_workday.requiresReport && dailyReport.isEmpty())) {
        setStatusLabel(m_workdayError, QStringLiteral("Заполните причину изменения времени и обязательный отчёт, если он нужен."));
        return;
    }
    const auto chosen = selectedTime();
    if (!chosen.isValid())
        return;
    beginWorkdayChange();
    m_backend->changeWorkdayAt(action, date->date(), QTime(chosen.hour(), chosen.minute()), explanation, dailyReport);
}

void WorktimePanel::showSelectedDay(const QModelIndex &index)
{
    if (!index.isValid()) {
        setStatusLabel(m_selection, m_table->isHidden() ? QString() : QStringLiteral("Выберите день, чтобы увидеть подробности."));
        return;
    }
    const auto &sheet = m_model->sheet();
    const auto *day = m_model->day(index);
    auto details = QStringLiteral("%1 · %2").arg(sheet.employees.at(index.row()).name,
        QLocale(QLocale::Russian).toString(sheet.month.addDays(index.column()), QStringLiteral("d MMMM yyyy")));
    details += day && !day->value.isEmpty() ? QStringLiteral(" · ") + day->value : QStringLiteral(" · Нет записи");
    if (day && !day->details.isEmpty())
        details += QLatin1Char('\n') + day->details;
    setStatusLabel(m_selection, details);
}

void WorktimePanel::chooseExport()
{
    if (!m_export->isEnabled())
        return;
    const auto epoch = m_accountEpoch;
    const auto month = m_month;
    QFileDialog save(this, QStringLiteral("Сохранить табель"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), QStringLiteral("Excel (*.xls)"));
    save.setAcceptMode(QFileDialog::AcceptSave);
    save.setDefaultSuffix(QStringLiteral("xls"));
    save.selectFile(QStringLiteral("Табель %1.xls").arg(monthKey(month)));
    if (save.exec() != QDialog::Accepted)
        return;
    const auto path = save.selectedFiles().value(0);
    if (path.isEmpty() || epoch != m_accountEpoch || month != m_month || !m_export->isEnabled())
        return;
    m_exportMonth = month;
    m_exportPath = path;
    setStatusLabel(m_exportStatus, QStringLiteral("Скачиваем табель…"), true);
    updateControls();
    m_backend->exportWorktimeSheet(month, path);
}

} // namespace bx::ui
