#include "ui/connection_dialog.h"

#include "ui/workspace_style.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyle>
#include <QVBoxLayout>

namespace bx {
namespace {

const auto connectionStyle = QStringLiteral(R"(
    QDialog#connectionDialog { background: #fffefa; }
    QLabel#connectionTitle { color: #182d3b; font-size: 28px; font-weight: 700; }
    QLabel#connectionHint { color: #516674; }
    QLabel#connectionCaption { color: #182d3b; font-weight: 650; }
    QLabel#connectionFeedback { color: #21536a; background: #e7f1f1; border-radius: 10px; padding: 10px 12px; }
    QLabel#connectionFeedback[error="true"] { color: #912c31; background: #fbecec; }
    QLineEdit#connectionAddress {
        color: #182d3b; background: #f7f8f3; border: 2px solid #dce4e4;
        border-radius: 11px; min-height: 25px; padding: 10px 12px;
        selection-color: #fff; selection-background-color: #215c70;
    }
    QLineEdit#connectionAddress:hover { border-color: #9eb8be; }
    QLineEdit#connectionAddress:focus { border-color: #3c8996; background: #fff; }
    QPushButton#connectionContinue {
        color: #fff; background: #215c70; border: 2px solid #215c70;
        border-radius: 11px; min-height: 25px; padding: 9px 14px;
        text-align: left; font-weight: 700;
    }
    QPushButton#connectionContinue:hover { background: #1a4e60; border-color: #1a4e60; }
    QPushButton#connectionContinue:pressed { background: #133f50; border-color: #133f50; }
    QPushButton#connectionContinue:focus { border-color: #7cbcba; }
    QPushButton#connectionContinue:disabled { color: #516674; background: #e7eceb; border-color: #e7eceb; }
    QPushButton#connectionCancel {
        color: #516674; background: transparent; border: 2px solid transparent;
        border-radius: 9px; padding: 7px 8px;
    }
    QPushButton#connectionCancel:hover {
        background: #edf2ef; color: #182d3b;
    }
    QPushButton#connectionCancel:focus { border-color: #8db2b6; }
    QPushButton#connectionCancel:pressed { background: #dde7e5; }
)");

class ConnectionIllustration final : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QLinearGradient panel(rect().topLeft(), rect().bottomRight());
        panel.setColorAt(0, QColor("#284b57"));
        panel.setColorAt(1, QColor("#192e42"));
        painter.fillRect(rect(), panel);

        const qreal scale = qMin(width() / 270.0, height() / 330.0);
        painter.translate(width() / 2.0, height() / 2.0);
        painter.scale(scale, scale);
        painter.translate(-135, -158);

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor("#507782"), 1));
        painter.drawEllipse(QPointF(135, 160), 105, 105);
        painter.setPen(QPen(QColor("#42636f"), 1));
        painter.drawEllipse(QPointF(135, 160), 126, 126);

        paintBrowser(painter);
        paintWorkspace(painter);
        paintTransfer(painter);
    }

private:
    static void paintBrowser(QPainter &painter)
    {
        const QRectF frame(39, 76, 151, 115);
        painter.setPen(QPen(QColor("#779ca4"), 1.3));
        painter.setBrush(QColor("#264758"));
        painter.drawRoundedRect(frame, 12, 12);
        painter.drawLine(QPointF(39, 101), QPointF(190, 101));

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#aac8c6"));
        for (const auto x : {53, 62, 71})
            painter.drawEllipse(QPointF(x, 89), 1.9, 1.9);
        painter.setPen(QPen(QColor("#92b5b7"), 1.3));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QPointF(96, 141), 24, 24);
        painter.drawEllipse(QPointF(96, 141), 10, 24);
        painter.drawLine(QPointF(74, 141), QPointF(118, 141));
    }

    static void paintWorkspace(QPainter &painter)
    {
        const QRectF frame(101, 144, 132, 106);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#142b3b"));
        painter.drawRoundedRect(frame.translated(0, 5), 12, 12);
        painter.setBrush(QColor("#e4f0ec"));
        painter.drawRoundedRect(frame, 12, 12);
        painter.setBrush(QColor("#c5ddda"));
        painter.drawRoundedRect(QRectF(111, 154, 20, 86), 6, 6);
        painter.setBrush(QColor("#69969a"));
        painter.drawRoundedRect(QRectF(142, 161, 58, 5), 2.5, 2.5);
        painter.setBrush(QColor("#b5d3ce"));
        painter.drawRoundedRect(QRectF(142, 175, 69, 13), 5, 5);
        painter.drawRoundedRect(QRectF(155, 195, 57, 13), 5, 5);
        painter.drawRoundedRect(QRectF(142, 215, 46, 12), 5, 5);
    }

    static void paintTransfer(QPainter &painter)
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#8ccec4"));
        painter.drawEllipse(QPointF(191, 132), 19, 19);
        painter.setPen(QPen(QColor("#204c57"), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(QPointF(183, 132), QPointF(198, 132));
        painter.drawPolyline(QPolygonF{QPointF(193, 127), QPointF(198, 132), QPointF(193, 137)});
    }
};

QIcon globeIcon()
{
    QPixmap pixmap(36, 36);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(workspace::Accent, 1.2));
    painter.drawEllipse(QPointF(9, 9), 6.25, 6.25);
    painter.drawEllipse(QPointF(9, 9), 2.8, 6.25);
    painter.drawLine(QPointF(2.75, 9), QPointF(15.25, 9));
    return QIcon(pixmap);
}

} // namespace

ConnectionDialog::ConnectionDialog(const QUrl &prefill, QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("connectionDialog"));
    setWindowTitle(tr("Битрикс24"));
    setStyleSheet(connectionStyle);
    setMinimumWidth(438);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_illustration = new ConnectionIllustration(this);
    m_illustration->setFixedWidth(254);
    layout->addWidget(m_illustration);

    auto *content = new QVBoxLayout;
    content->setContentsMargins(36, 56, 36, 22);
    content->setSpacing(0);
    layout->addLayout(content, 1);
    content->addWidget(workspace::label(tr("Подключение к порталу"), "connectionTitle", this));
    content->addSpacing(9);
    content->addWidget(workspace::label(tr("Откроется страница входа Битрикс24 в Chrome. Выберите удобный способ входа."),
                                        "connectionHint", this));
    content->addSpacing(25);

    auto *caption = workspace::label(tr("&Адрес портала"), "connectionCaption", this);
    content->addWidget(caption);
    content->addSpacing(7);

    m_address = new QLineEdit(this);
    m_address->setObjectName(QStringLiteral("connectionAddress"));
    m_address->setAccessibleName(tr("Адрес портала"));
    m_address->setPlaceholderText(QStringLiteral("company.bitrix24.ru"));
    m_address->setMaxLength(2048);
    m_address->setInputMethodHints(Qt::ImhUrlCharactersOnly | Qt::ImhNoPredictiveText);
    auto addressPalette = m_address->palette();
    addressPalette.setColor(QPalette::PlaceholderText, workspace::Muted);
    m_address->setPalette(addressPalette);
    caption->setBuddy(m_address);
    m_address->addAction(globeIcon(), QLineEdit::LeadingPosition);
    if (!prefill.isEmpty()) {
        auto displayAddress = prefill.toDisplayString(QUrl::StripTrailingSlash);
        if (displayAddress.startsWith(QLatin1String("https://")))
            displayAddress.remove(0, 8);
        m_address->setText(displayAddress);
    }
    content->addWidget(m_address);
    content->addSpacing(9);

    m_feedback = workspace::label({}, "connectionFeedback", this);
    m_feedback->setAccessibleName(tr("Состояние подключения"));
    m_feedback->hide();
    m_continue = new QPushButton(this);
    m_continue->setObjectName(QStringLiteral("connectionContinue"));
    m_continue->setDefault(true);
    m_continue->setCursor(Qt::PointingHandCursor);
    resetFeedback();
    content->addWidget(m_continue);
    content->addSpacing(9);
    content->addWidget(m_feedback);
    content->addStretch();

    auto *cancel = new QPushButton(tr("Отмена"), this);
    cancel->setObjectName(QStringLiteral("connectionCancel"));
    cancel->setAutoDefault(false);
    cancel->setCursor(Qt::PointingHandCursor);
    content->addWidget(cancel, 0, Qt::AlignRight);

    setTabOrder(m_address, m_continue);
    setTabOrder(m_continue, cancel);
    connect(m_continue, &QPushButton::clicked, this, &ConnectionDialog::submit);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_address, &QLineEdit::textEdited, this, [this] {
        resetFeedback();
        emit addressEdited();
    });
    resize(744, 414);
    m_address->setFocus();
}

QUrl ConnectionDialog::normalizePortal(const QString &address)
{
    auto text = address.trimmed();
    if (text.isEmpty() || text.contains(QChar::LineFeed) || text.contains(QChar::CarriageReturn))
        return {};
    if (!text.contains(QStringLiteral("://")))
        text.prepend(QStringLiteral("https://"));
    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("https") || url.host().isEmpty()
        || url.authority().contains(QLatin1Char('@')) || url.port() == 0)
        return {};

    auto origin = url.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
    if (origin.port() == 443)
        origin.setPort(-1);
    return origin;
}

QUrl ConnectionDialog::portal() const
{
    return normalizePortal(m_address->text());
}

void ConnectionDialog::setCheckingSavedSession()
{
    m_continue->setText(tr("Войти"));
    m_continue->setEnabled(false);
    showFeedback(tr("Проверяем сохранённый вход."), false, true);
}

void ConnectionDialog::setWaiting(const QString &message)
{
    m_continue->setEnabled(true);
    m_continue->setText(tr("Открыть браузер снова"));
    showFeedback(message.isEmpty()
        ? tr("Завершите вход в Битрикс24. Клиент откроется автоматически.")
        : message, false);
}

void ConnectionDialog::setError(const QString &message)
{
    m_continue->setEnabled(true);
    m_continue->setText(tr("Войти"));
    showFeedback(message, true);
}

void ConnectionDialog::resetFeedback()
{
    ui::setLoadingIndicator(m_feedback, false);
    m_feedback->hide();
    m_continue->setEnabled(true);
    m_continue->setText(tr("Войти"));
}

void ConnectionDialog::showFeedback(const QString &message, bool error, bool loading)
{
    m_feedback->setProperty("error", error);
    m_feedback->style()->unpolish(m_feedback);
    m_feedback->style()->polish(m_feedback);
    m_feedback->setText(message);
    ui::setLoadingIndicator(m_feedback, loading);
    m_feedback->setVisible(!message.isEmpty());
}

void ConnectionDialog::submit()
{
    if (!m_continue->isEnabled())
        return;
    const auto origin = portal();
    if (origin.isEmpty()) {
        setError(tr("Введите адрес портала с HTTPS, например company.bitrix24.ru."));
        m_address->setFocus();
        return;
    }
    emit connectRequested(origin);
}

void ConnectionDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    m_illustration->setVisible(event->size().width() >= 674);
}

} // namespace bx
