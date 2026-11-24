#include "ui/public_link_preview_p.h"
#include "ui/chat_image_previews.h"

#include <QHash>
#include <QHostInfo>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSet>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

namespace bx::ui {
namespace {

constexpr qsizetype prefixLimit = 128 * 1024;
constexpr qsizetype thumbnailLimit = 1536 * 1024;
constexpr int pageDeadlineMs = 5000;
constexpr int thumbnailDeadlineMs = 8000;

bool isThumbnailType(const QByteArray &type)
{
    return type == "image/jpeg" || type == "image/png" || type == "image/webp" || type == "image/gif";
}

bool supportsPinnedTls()
{
#ifdef Q_OS_WIN
    // Qt may fetch certificate issuer URLs on Windows during verification.
    return false;
#else
    return QSslSocket::supportsSsl() && QSslSocket::activeBackend() == QLatin1String("openssl");
#endif
}

bool isPublic(const QHostAddress &address)
{
    using Network = QPair<QHostAddress, int>;
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        // IANA special-use assignments, plus multicast and reserved high ranges.
        static const auto blocked = [] {
            QList<Network> networks;
            for (const auto *cidr : {"0.0.0.0/8", "10.0.0.0/8", "100.64.0.0/10", "127.0.0.0/8",
                                     "169.254.0.0/16", "172.16.0.0/12", "192.0.0.0/24", "192.0.2.0/24",
                                     "192.31.196.0/24", "192.52.193.0/24", "192.88.99.0/24", "192.168.0.0/16",
                                     "192.175.48.0/24", "198.18.0.0/15", "198.51.100.0/24", "203.0.113.0/24", "224.0.0.0/3"})
                networks.append(QHostAddress::parseSubnet(QLatin1String(cidr)));
            return networks;
        }();
        return std::none_of(blocked.cbegin(), blocked.cend(), [&address](const Network &network) {
            return address.isInSubnet(network);
        });
    }
    if (address.protocol() != QAbstractSocket::IPv6Protocol || !address.scopeId().isEmpty())
        return false;
    static const auto global = QHostAddress::parseSubnet(QStringLiteral("2000::/3"));
    static const auto blocked = [] {
        QList<Network> networks;
        for (const auto *cidr : {"2001::/23", "2001:db8::/32", "2002::/16", "2620:4f:8000::/48", "3fff::/20"})
            networks.append(QHostAddress::parseSubnet(QLatin1String(cidr)));
        return networks;
    }();
    return address.isInSubnet(global)
        && std::none_of(blocked.cbegin(), blocked.cend(), [&address](const Network &network) {
            return address.isInSubnet(network);
        });
}

QString plainMetadata(const QString &raw, qsizetype limit)
{
    static const QRegularExpression entity(QStringLiteral("&(#x[0-9a-f]{1,8}|#[0-9]{1,8}|[a-z]{2,8});"),
                                            QRegularExpression::CaseInsensitiveOption);
    static const QHash<QString, QString> named{{QStringLiteral("amp"), QStringLiteral("&")},
        {QStringLiteral("quot"), QStringLiteral("\"")}, {QStringLiteral("apos"), QStringLiteral("'")},
        {QStringLiteral("lt"), QStringLiteral("<")}, {QStringLiteral("gt"), QStringLiteral(">")},
        {QStringLiteral("nbsp"), QStringLiteral(" ")}, {QStringLiteral("mdash"), QStringLiteral("—")},
        {QStringLiteral("ndash"), QStringLiteral("–")}, {QStringLiteral("hellip"), QStringLiteral("…")}};
    const auto input = raw.left(limit * 12);
    QString result;
    qsizetype cursor = 0;
    auto matches = entity.globalMatch(input);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += input.mid(cursor, match.capturedStart() - cursor);
        const auto symbol = match.captured(1).toLower();
        if (symbol.startsWith(QLatin1Char('#'))) {
            const bool hex = symbol.startsWith(QLatin1String("#x"));
            bool valid = false;
            const char32_t point = symbol.mid(hex ? 2 : 1).toUInt(&valid, hex ? 16 : 10);
            if (valid && point > 0 && point <= 0x10ffff && !(point >= 0xd800 && point <= 0xdfff))
                result += QString::fromUcs4(&point, 1);
        } else {
            result += named.value(symbol, match.captured());
        }
        cursor = match.capturedEnd();
    }
    result += input.mid(cursor);
    static const QRegularExpression controls(QStringLiteral("[\\x{0000}-\\x{001f}\\x{007f}-\\x{009f}"
                                                             "\\x{200e}\\x{200f}\\x{202a}-\\x{202e}\\x{2066}-\\x{2069}]"));
    result = result.replace(controls, QStringLiteral(" ")).simplified().left(limit);
    if (!result.isEmpty() && result.back().isHighSurrogate())
        result.chop(1);
    return result;
}

std::pair<qsizetype, qsizetype> rawTextEnd(const QString &html, const QString &tag, qsizetype from)
{
    const auto prefix = QStringLiteral("</") + tag;
    for (qsizetype at = html.indexOf(prefix, from, Qt::CaseInsensitive); at >= 0;
         at = html.indexOf(prefix, at + prefix.size(), Qt::CaseInsensitive)) {
        const qsizetype after = at + prefix.size();
        if (after < html.size() && (html[after].isSpace() || html[after] == QLatin1Char('>') || html[after] == QLatin1Char('/')))
            return {at, html.indexOf(QLatin1Char('>'), after)};
    }
    return {-1, -1};
}

} // namespace

namespace public_link_preview {

QByteArray publicHttpsHost(const QUrl &url)
{
    if (!url.isValid() || url.scheme() != QLatin1String("https") || url.port(443) != 443
        || url.authority(QUrl::FullyEncoded).contains(QLatin1Char('@')) || url.toEncoded().size() > 2048)
        return {};
    const auto host = QUrl::toAce(url.host()).toLower();
    if (host.size() > 253 || QHostAddress().setAddress(QString::fromLatin1(host)))
        return {};
    static const QRegularExpression dns(QStringLiteral("\\A(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\\.)+"
                                                       "[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?\\z"));
    if (!dns.match(QString::fromLatin1(host)).hasMatch())
        return {};
    static const QSet<QByteArray> reserved{"alt", "arpa", "corp", "example", "home", "internal", "invalid", "lan",
                                          "local", "localhost", "onion", "test"};
    if (reserved.contains(host.mid(host.lastIndexOf('.') + 1)))
        return {};
    static const QSet<QString> secretKeys{
        QStringLiteral("auth"), QStringLiteral("authorization"), QStringLiteral("bearer"), QStringLiteral("token"),
        QStringLiteral("accesstoken"), QStringLiteral("refreshtoken"), QStringLiteral("idtoken"), QStringLiteral("oauthtoken"),
        QStringLiteral("oauthtokensecret"), QStringLiteral("bearertoken"), QStringLiteral("apitoken"),
        QStringLiteral("apikey"), QStringLiteral("accesskey"), QStringLiteral("accesskeyid"), QStringLiteral("secretkey"),
        QStringLiteral("password"), QStringLiteral("passwd"), QStringLiteral("secret"), QStringLiteral("clientsecret"),
        QStringLiteral("session"), QStringLiteral("sessionid"), QStringLiteral("sessid"), QStringLiteral("csrf"),
        QStringLiteral("csrftoken"), QStringLiteral("xcsrftoken"), QStringLiteral("xsrftoken"),
        QStringLiteral("sig"), QStringLiteral("signature"), QStringLiteral("xamzsignature"), QStringLiteral("xgoogsignature"),
        QStringLiteral("xamzcredential"), QStringLiteral("xgoogcredential"), QStringLiteral("xamzsecuritytoken"),
        QStringLiteral("keypairid")};
    for (const auto &[key, ignored] : QUrlQuery(url).queryItems(QUrl::FullyDecoded)) {
        auto normalized = key.toLower();
        normalized.remove(QLatin1Char('-')).remove(QLatin1Char('_'));
        if (secretKeys.contains(normalized))
            return {};
    }
    return host;
}

std::optional<QHostAddress> publicAddress(const QList<QHostAddress> &addresses)
{
    if (addresses.isEmpty() || std::any_of(addresses.cbegin(), addresses.cend(), [](const auto &address) {
            return !isPublic(address);
        }))
        return std::nullopt;
    const auto ipv4 = std::find_if(addresses.cbegin(), addresses.cend(), [](const auto &address) {
        return address.protocol() == QAbstractSocket::IPv4Protocol;
    });
    return ipv4 == addresses.cend() ? addresses.first() : *ipv4;
}

QNetworkRequest pinnedRequest(const QUrl &url, const QByteArray &host, const QHostAddress &address, bool image)
{
    auto endpoint = url;
    endpoint.setHost(address.toString());
    endpoint.setFragment({});
    QNetworkRequest request(endpoint);
    request.setPeerVerifyName(QString::fromLatin1(host));
    auto tls = QSslConfiguration::defaultConfiguration();
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    tls.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(tls);
    request.setPriority(QNetworkRequest::LowPriority);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setRawHeader("Host", host);
    // Some public sites return preview metadata only to clients identifying as a bot.
    request.setRawHeader("User-Agent", "Bitrix24Client-LinkPreviewBot/1.0");
    request.setRawHeader("Accept", image ? "image/jpeg,image/png,image/webp,image/gif" : "text/html,application/xhtml+xml");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setRawHeader("Connection", "close");
    return request;
}

QUrl thumbnailTarget(const QUrl &base, const QByteArray &reference)
{
    if (reference.isEmpty() || reference.size() > 2048)
        return {};
    const auto target = QUrl::fromEncoded(reference, QUrl::StrictMode);
    if (!target.isValid())
        return {};
    const auto resolved = base.resolved(target).adjusted(QUrl::RemoveFragment);
    return publicHttpsHost(resolved).isEmpty() ? QUrl{} : resolved;
}

std::optional<HtmlMetadata> htmlMetadata(const QByteArray &bytes, const QUrl &page)
{
    const auto html = QString::fromUtf8(bytes.left(prefixLimit));
    static const QRegularExpression token(QStringLiteral(
        R"re(<!--[\s\S]*?(?:-->|\z)|<(/?)([a-z][a-z0-9:-]*)(?=[\s/>])((?:[^<>'"]|"[^"]*"|'[^']*')*)>)re"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression attr(QStringLiteral(
        R"re(([^\s=/>]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`>]+)))?)re"));
    QString ogTitle, twitterTitle, title, ogDescription, twitterDescription, description;
    QUrl secureImage, ogImage, twitterImage;
    int templateDepth = 0;
    qsizetype cursor = 0;
    while (cursor < html.size()) {
        const auto tag = token.match(html, cursor);
        if (!tag.hasMatch())
            break;
        cursor = tag.capturedEnd();
        const auto name = tag.captured(2).toLower();
        const bool closing = !tag.captured(1).isEmpty();
        if (name.isEmpty())
            continue;
        if (name == QLatin1String("template")) {
            templateDepth = qMax(0, templateDepth + (closing ? -1 : 1));
            continue;
        }
        if (!closing && (name == QLatin1String("script") || name == QLatin1String("style") || name == QLatin1String("title")
                         || name == QLatin1String("noscript") || name == QLatin1String("noframes"))) {
            const auto [textEnd, tagEnd] = rawTextEnd(html, name, cursor);
            if (tagEnd < 0)
                break;
            if (name == QLatin1String("title") && title.isEmpty() && templateDepth == 0)
                title = plainMetadata(html.mid(cursor, textEnd - cursor), 250);
            cursor = tagEnd + 1;
            continue;
        }
        if (templateDepth > 0)
            continue;
        if (closing && (name == QLatin1String("head") || name == QLatin1String("html")))
            break;
        if (!closing && name == QLatin1String("meta")) {
            const auto body = tag.captured(3);
            QHash<QString, QString> values;
            auto attributes = attr.globalMatch(body);
            while (attributes.hasNext()) {
                const auto pair = attributes.next();
                const auto key = pair.captured(1).toLower();
                if ((key == QLatin1String("property") || key == QLatin1String("name") || key == QLatin1String("content"))
                    && !values.contains(key))
                    values.insert(key, pair.captured(2) + pair.captured(3) + pair.captured(4));
            }
            const auto kind = values.value(QStringLiteral("property"), values.value(QStringLiteral("name"))).trimmed().toLower();
            const auto content = values.value(QStringLiteral("content"));
            if (kind == QLatin1String("og:title") && ogTitle.isEmpty())
                ogTitle = plainMetadata(content, 250);
            else if (kind == QLatin1String("og:description") && ogDescription.isEmpty())
                ogDescription = plainMetadata(content, 350);
            else if (kind == QLatin1String("twitter:title") && twitterTitle.isEmpty())
                twitterTitle = plainMetadata(content, 250);
            else if (kind == QLatin1String("twitter:description") && twitterDescription.isEmpty())
                twitterDescription = plainMetadata(content, 350);
            else if (kind == QLatin1String("description") && description.isEmpty())
                description = plainMetadata(content, 350);
            else if (content.size() <= 2048) {
                QUrl *image = nullptr;
                if (kind == QLatin1String("og:image:secure_url"))
                    image = &secureImage;
                else if (kind == QLatin1String("og:image") || kind == QLatin1String("og:image:url"))
                    image = &ogImage;
                else if (kind == QLatin1String("twitter:image") || kind == QLatin1String("twitter:image:src"))
                    image = &twitterImage;
                if (image && image->isEmpty())
                    *image = thumbnailTarget(page, plainMetadata(content, 2048).toUtf8());
            }
            continue;
        }
        if (!closing && name != QLatin1String("html") && name != QLatin1String("head")
            && name != QLatin1String("base") && name != QLatin1String("link"))
            break;
    }
    HtmlMetadata data{
        !ogTitle.isEmpty() ? ogTitle : !twitterTitle.isEmpty() ? twitterTitle : title,
        !ogDescription.isEmpty() ? ogDescription : !twitterDescription.isEmpty() ? twitterDescription : description,
        !secureImage.isEmpty() ? secureImage : !ogImage.isEmpty() ? ogImage : twitterImage,
    };
    if (data.title.isEmpty() && data.description.isEmpty() && data.thumbnailUrl.isEmpty())
        return std::nullopt;
    return data;
}

QImage decodeThumbnail(const QByteArray &bytes, const QByteArray &contentType)
{
    if (bytes.size() > thumbnailLimit || !isThumbnailType(contentType))
        return {};
    return decodeChatPreviewRaster(bytes, QSize(160, 120), contentType).convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

} // namespace public_link_preview

PublicLinkPreviewFetcher::PublicLinkPreviewFetcher(QObject *parent) : QObject(parent)
{
    m_deadline.setSingleShot(true);
    m_deadline.setTimerType(Qt::PreciseTimer);
    connect(&m_deadline, &QTimer::timeout, this, &PublicLinkPreviewFetcher::finish);
}

PublicLinkPreviewFetcher::~PublicLinkPreviewFetcher()
{
    if (m_lookup >= 0)
        QHostInfo::abortHostLookup(m_lookup);
}

bool PublicLinkPreviewFetcher::accepts(const QUrl &url)
{
    return !public_link_preview::publicHttpsHost(url).isEmpty() && supportsPinnedTls();
}

bool PublicLinkPreviewFetcher::request(const QUrl &url, Completion complete)
{
    if (!complete || m_complete || m_lookup >= 0 || !supportsPinnedTls() || !resolve(url))
        return false;
    m_complete = std::move(complete);
    m_imageRedirects = 0;
    m_deadline.start(pageDeadlineMs);
    return true;
}

bool PublicLinkPreviewFetcher::resolve(const QUrl &url)
{
    const auto host = public_link_preview::publicHttpsHost(url);
    if (host.isEmpty())
        return false;
    // An absolute DNS name prevents resolver search-domain substitutions.
    m_lookup = QHostInfo::lookupHost(QString::fromLatin1(host) + QLatin1Char('.'), this,
        [this, url, host](const QHostInfo &info) {
            if (m_lookup != info.lookupId())
                return;
            m_lookup = -1;
            // A resolver call can outlive the visible timeout. It never starts a late request.
            if (!m_complete)
                return;
            const auto addresses = info.addresses();
            const auto local = QNetworkInterface::allAddresses();
            const auto address = public_link_preview::publicAddress(addresses);
            if (info.error() != QHostInfo::NoError || !address ||
                std::any_of(addresses.cbegin(), addresses.cend(), [&local](const auto &answer) { return local.contains(answer); })) {
                finish();
                return;
            }
            startGet(url, host, *address);
        });
    return m_lookup >= 0;
}

void PublicLinkPreviewFetcher::startGet(const QUrl &url, const QByteArray &host, const QHostAddress &address)
{
    // A fresh manager prevents reuse of a TLS connection keyed by a shared CDN IP.
    m_network = new QNetworkAccessManager(this);
    m_network->setProxy(QNetworkProxy::NoProxy);
    m_url = url;
    auto *reply = m_network->get(public_link_preview::pinnedRequest(url, host, address, m_metadata.has_value()));
    m_reply = reply;
    reply->setReadBufferSize(m_metadata ? 128 * 1024 : 16 * 1024);
    connect(reply, &QNetworkReply::metaDataChanged, this, &PublicLinkPreviewFetcher::readPrefix);
    connect(reply, &QIODevice::readyRead, this, &PublicLinkPreviewFetcher::readPrefix);
    connect(reply, &QNetworkReply::finished, this, &PublicLinkPreviewFetcher::readPrefix);
}

void PublicLinkPreviewFetcher::readPrefix()
{
    auto *reply = m_reply;
    if (!reply)
        return;
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (!status.isValid() && !reply->isFinished())
        return;
    const auto code = status.toInt();
    const bool image = m_metadata.has_value();
    if (image && (code == 301 || code == 302 || code == 303 || code == 307 || code == 308)) {
        const auto target = public_link_preview::thumbnailTarget(m_url, reply->rawHeader("Location"));
        clearTransfer();
        if (++m_imageRedirects > 2 || !resolve(target))
            finish();
        return;
    }
    const auto type = reply->rawHeader("Content-Type").split(';').first().trimmed().toLower();
    const auto encoding = reply->rawHeader("Content-Encoding").trimmed().toLower();
    const bool supportedType = image ? isThumbnailType(type) : type == "text/html" || type == "application/xhtml+xml";
    if (code != 200 || !supportedType
        || (!encoding.isEmpty() && encoding != "identity") || reply->error() != QNetworkReply::NoError) {
        finish();
        return;
    }
    if (image) {
        if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > thumbnailLimit) {
            finish();
            return;
        }
        m_prefix += reply->read(thumbnailLimit + 1 - m_prefix.size());
        if (m_prefix.size() > thumbnailLimit) {
            finish();
        } else if (reply->isFinished()) {
            m_metadata->thumbnail = public_link_preview::decodeThumbnail(m_prefix, type);
            finish();
        }
        return;
    }
    if (reply->bytesAvailable() > 0)
        m_prefix += reply->read(prefixLimit - m_prefix.size());
    if (m_prefix.size() < prefixLimit && !reply->isFinished())
        return;
    const auto page = public_link_preview::htmlMetadata(m_prefix, m_url);
    if (!page) {
        finish();
        return;
    }
    m_metadata = PublicLinkMetadata{page->title, page->description};
    clearTransfer();
    if (!page->thumbnailUrl.isEmpty() && resolve(page->thumbnailUrl))
        m_deadline.start(thumbnailDeadlineMs);
    else
        finish();
}

void PublicLinkPreviewFetcher::clearTransfer()
{
    if (auto *reply = std::exchange(m_reply, nullptr)) {
        reply->disconnect(this);
        if (!reply->isFinished())
            reply->abort();
    }
    if (auto *network = std::exchange(m_network, nullptr)) {
        network->clearConnectionCache();
        network->deleteLater();
    }
    m_prefix.clear();
}

void PublicLinkPreviewFetcher::finish()
{
    m_deadline.stop();
    clearTransfer();
    auto result = std::exchange(m_metadata, std::nullopt);
    if (result && result->title.isEmpty() && result->description.isEmpty() && result->thumbnail.isNull())
        result.reset();
    auto complete = std::move(m_complete);
    m_complete = {};
    if (complete)
        complete(std::move(result));
}

} // namespace bx::ui
