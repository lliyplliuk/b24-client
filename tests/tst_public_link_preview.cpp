#include "ui/public_link_preview_p.h"

#include <QBuffer>
#include <QRandomGenerator>
#include <QTest>

using namespace bx::ui;
using namespace bx::ui::public_link_preview;

class TestPublicLinkPreview : public QObject {
    Q_OBJECT

private slots:
    void validatesHostAndResolvedAddress();
    void rejectsCredentialsAndSignaturesInQueryKeys();
    void buildsAnAnonymousPinnedRequest();
    void extractsInertHeadMetadata();
    void recognizesImageOnlyPreview();
    void validatesImageAndRedirectUrls();
    void decodesBoundedRaster();
    void decodesModerateSizeJpeg();
};

void TestPublicLinkPreview::validatesHostAndResolvedAddress()
{
    QCOMPARE(publicHttpsHost(QUrl(QStringLiteral("https://playerok.com/products/one"))), QByteArray("playerok.com"));
    for (const auto *url : {"http://public.net/", "https://127.1/", "https://2130706433/", "https://[::1]/",
                            "https://printer.local/", "https://router.home.arpa/", "https://portal.internal/",
                            "https://public.net:8443/", "https://user@public.net/", "https://@public.net/",
                            "https://public.net./", "https://public.net/?access_token=secret", "https://localhost/"})
        QVERIFY2(publicHttpsHost(QUrl(QString::fromLatin1(url), QUrl::StrictMode)).isEmpty(), url);
    const QHostAddress ipv4(QStringLiteral("34.120.121.122"));
    const QHostAddress ipv6(QStringLiteral("2606:4700:4700::1111"));
    QCOMPARE(publicAddress({ipv6, ipv4}), std::optional<QHostAddress>(ipv4));
    QCOMPARE(publicAddress({ipv6}), std::optional<QHostAddress>(ipv6));
    for (const auto *ip : {"10.1.2.3", "100.64.0.1", "127.0.0.2", "169.254.169.254", "172.16.1.1", "192.168.1.1",
                           "192.88.99.1", "192.0.2.1", "198.18.0.1", "224.0.0.1", "240.0.0.1", "::ffff:34.120.121.122",
                           "::1", "fd00::1", "fe80::1", "64:ff9b::127.0.0.1", "2001::1", "2001:db8::1", "3fff::1"}) {
        QVERIFY2(!publicAddress({QHostAddress(QString::fromLatin1(ip))}), ip);
        QVERIFY2(!publicAddress({ipv4, QHostAddress(QString::fromLatin1(ip))}), ip);
    }
    PublicLinkPreviewFetcher transport;
    QVERIFY(!transport.request(QUrl(QStringLiteral("https://127.0.0.1")), [](auto) { QFAIL("Rejected request completed"); }));
}

void TestPublicLinkPreview::rejectsCredentialsAndSignaturesInQueryKeys()
{
    for (const auto *query : {"To%4bEn=local-fixture", "id%5Ftoken=local-fixture", "OAuth-Token=local-fixture",
                              "bearer_token=local-fixture", "API-Key=local-fixture", "AcCeSs-KeY=local-fixture",
                              "passWd=local-fixture", "secret=local-fixture", "session-ID=local-fixture",
                              "CSRF_token=local-fixture", "s%69g=local-fixture", "sigNature=local-fixture",
                              "X-Amz%2DSignature=local-fixture", "x_GOOG-SIGNATURE=local-fixture",
                              "Key-Pair-Id=local-fixture", "v=video&Token="}) {
        const QUrl url(QStringLiteral("https://public.net/?") + QString::fromLatin1(query), QUrl::StrictMode);
        QVERIFY2(publicHttpsHost(url).isEmpty(), query);
    }
    const QUrl normal(QStringLiteral("https://www.youtube.com/watch?v=abc&q=access_token&signature_type=none"),
                      QUrl::StrictMode);
    QCOMPARE(publicHttpsHost(normal), QByteArray("www.youtube.com"));
}

void TestPublicLinkPreview::buildsAnAnonymousPinnedRequest()
{
    const QUrl url(QStringLiteral("https://playerok.com/p?q=a%2Fb#not-sent"), QUrl::StrictMode);
    const auto request = pinnedRequest(url, "playerok.com", QHostAddress(QStringLiteral("34.120.121.122")));
    QCOMPARE(request.url().toEncoded(), QByteArray("https://34.120.121.122/p?q=a%2Fb"));
    QCOMPARE(request.peerVerifyName(), QStringLiteral("playerok.com"));
    QCOMPARE(request.rawHeader("Host"), QByteArray("playerok.com"));
    QCOMPARE(request.rawHeader("User-Agent"), QByteArray("Bitrix24Client-LinkPreviewBot/1.0"));
    QCOMPARE(request.rawHeader("Accept-Encoding"), QByteArray("identity"));
    QVERIFY(!request.hasRawHeader("Cookie"));
    QVERIFY(!request.hasRawHeader("Authorization"));
    QVERIFY(!request.hasRawHeader("Referer"));
    QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
             static_cast<int>(QNetworkRequest::ManualRedirectPolicy));
    QCOMPARE(request.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(), static_cast<int>(QNetworkRequest::Manual));
    QCOMPARE(request.attribute(QNetworkRequest::CookieSaveControlAttribute).toInt(), static_cast<int>(QNetworkRequest::Manual));
    const auto image = pinnedRequest(url, "playerok.com", QHostAddress(QStringLiteral("34.120.121.122")), true);
    QCOMPARE(image.rawHeader("Accept"), QByteArray("image/jpeg,image/png,image/webp,image/gif"));
    QCOMPARE(image.rawHeader("User-Agent"), request.rawHeader("User-Agent"));
    for (const auto *header : {"Cookie", "Authorization", "Referer"})
        QVERIFY(!image.hasRawHeader(header));
}

void TestPublicLinkPreview::extractsInertHeadMetadata()
{
    const auto metadata = htmlMetadata(R"(<!doctype html><HTML><HEAD>
        <!-- <meta property="og:title" content="comment"> -->
        <script>let spoof = '<meta property="og:title" content="script">';</script>
        <template><meta property="og:title" content="template"></template>
        <title>Fallback title</title>
        <meta content='Товар &amp; проверка &#x2713;' PROPERTY="OG:TITLE">
        <meta NAME=description content="Описание &gt; подробности">
        <meta property="og:image" content="http://127.0.0.1/test">
        </HEAD><body><meta property="og:description" content="body"></body></HTML>)");
    QVERIFY(metadata);
    QCOMPARE(metadata->title, QStringLiteral("Товар & проверка ✓"));
    QCOMPARE(metadata->description, QStringLiteral("Описание > подробности"));
    QVERIFY(!htmlMetadata(R"(<html><head><!-- <title>Hidden</title>)"));
    QVERIFY(!htmlMetadata(R"(<html><body><title>Body only</title></body>)"));
    const auto fallback = htmlMetadata("<title>Simple &quot;name&quot;</title>");
    QVERIFY(fallback);
    QCOMPARE(fallback->title, QStringLiteral("Simple \"name\""));
}

void TestPublicLinkPreview::recognizesImageOnlyPreview()
{
    const auto metadata = htmlMetadata("<head><meta property='og:image' content='https://cdn.public.net/card.jpg'></head>");
    QVERIFY(metadata);
    QCOMPARE(metadata->thumbnailUrl, QUrl(QStringLiteral("https://cdn.public.net/card.jpg")));
}

void TestPublicLinkPreview::validatesImageAndRedirectUrls()
{
    const QUrl page(QStringLiteral("https://www.public.net/store/item"));
    const auto metadata = htmlMetadata(R"re(<head>
        <script>let spoof = '<meta property="og:image:secure_url" content="/spoof.jpg">';</script>
        <meta property="og:image:secure_url" content="https://127.0.0.1/private">
        <meta property="og:image" content="../card.png?w=160&amp;h=120#discard">
        <meta name="twitter:image" content="/alt.png">
        <meta name="twitter:title" content="Product">
        </head>)re", page);
    QVERIFY(metadata);
    QCOMPARE(metadata->title, QStringLiteral("Product"));
    QCOMPARE(metadata->thumbnailUrl, QUrl(QStringLiteral("https://www.public.net/card.png?w=160&h=120")));
    const auto twitter = htmlMetadata("<meta name='twitter:image' content='//cdn.public.net/p.png'>", page);
    QVERIFY(twitter);
    QCOMPARE(twitter->thumbnailUrl, QUrl(QStringLiteral("https://cdn.public.net/p.png")));
    QCOMPARE(thumbnailTarget(metadata->thumbnailUrl, "/redirected.png"), QUrl(QStringLiteral("https://www.public.net/redirected.png")));
    for (const auto *target : {"https://lan.local/p", "https://127.0.0.1/p", "http://cdn.public.net/p",
                               "https://user@cdn.public.net/p", "https://cdn.public.net/p?access_token=secret",
                               "data:image/png;base64,AAAA", "https://cdn.public.net:9443/p"})
        QVERIFY2(thumbnailTarget(page, target).isEmpty(), target);
}

void TestPublicLinkPreview::decodesBoundedRaster()
{
    const auto png = [](QSize size) {
        QImage image(size, QImage::Format_RGB32);
        image.fill(QColor(209, 61, 100));
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        return bytes;
    };
    const auto valid = png(QSize(1200, 800));
    QVERIFY(!valid.isEmpty());
    const auto image = decodeThumbnail(valid, "image/png");
    QCOMPARE(image.size(), QSize(160, 106));
    QCOMPARE(image.pixelColor(80, 50), QColor(209, 61, 100));
    QVERIFY(decodeThumbnail(valid, "image/jpeg").isNull());
    QVERIFY(decodeThumbnail(valid, "image/svg+xml").isNull());
    QVERIFY(decodeThumbnail(valid.left(20), "image/png").isNull());
    QVERIFY(decodeThumbnail(png(QSize(4097, 1)), "image/png").isNull());
    QVERIFY(decodeThumbnail(png(QSize(2600, 1800)), "image/png").isNull());
    QVERIFY(decodeThumbnail(valid + QByteArray(1536 * 1024, 'x'), "image/png").isNull());
}

void TestPublicLinkPreview::decodesModerateSizeJpeg()
{
    QRandomGenerator random(8120);
    QImage original(QSize(1500, 1000), QImage::Format_RGB32);
    for (int y = 0; y < original.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(original.scanLine(y));
        for (int x = 0; x < original.width(); ++x)
            row[x] = random.generate() | 0xff000000;
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(original.save(&buffer, "JPEG", 87));
    QVERIFY(bytes.size() > 768 * 1024);
    QVERIFY(bytes.size() < 1536 * 1024);
    QCOMPARE(decodeThumbnail(bytes, "image/jpeg").size(), QSize(160, 106));
}

QTEST_GUILESS_MAIN(TestPublicLinkPreview)
#include "tst_public_link_preview.moc"
