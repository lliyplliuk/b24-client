#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <optional>

class QHostAddress;
class QNetworkAccessManager;
class QNetworkReply;

namespace bx::ui {

struct PublicLinkMetadata {
    QString title;
    QString description;
    QImage thumbnail{};
};

class PublicLinkPreviewFetcher final : public QObject {
public:
    using Completion = std::function<void(std::optional<PublicLinkMetadata>)>;

    explicit PublicLinkPreviewFetcher(QObject *parent = nullptr);
    ~PublicLinkPreviewFetcher() override;

    static bool accepts(const QUrl &url, bool requireNativeTransport = true);

    // False means the URL was rejected or another request is already active.
    bool request(const QUrl &url, Completion complete);

private:
    bool resolve(const QUrl &url);
    void startGet(const QUrl &url, const QByteArray &host, const QHostAddress &address);
    void readPrefix();
    void clearTransfer();
    void finish();

    QTimer m_deadline;
    int m_lookup = -1;
    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_reply = nullptr;
    QByteArray m_prefix;
    QUrl m_url;
    int m_imageRedirects = 0;
    std::optional<PublicLinkMetadata> m_metadata;
    Completion m_complete;
};

} // namespace bx::ui
