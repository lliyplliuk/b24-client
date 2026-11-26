#include "pilot_fixture.h"

using namespace bx;
using namespace bx::test;

class UiChatMediaTest : public PilotFixture {
    Q_OBJECT

private slots:
    void forwardedVideoShowsPosterAndOpensLocalOriginal_data()
    {
        QTest::addColumn<bool>("clickWhileDownloading");
        QTest::newRow("poster-only-until-click") << false;
        QTest::newRow("click-before-poster") << true;
    }

    void forwardedVideoShowsPosterAndOpensLocalOriginal()
    {
        QFETCH(bool, clickWhileDownloading);
        const auto ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (ffmpeg.isEmpty())
            QSKIP("FFmpeg is optional for automatic video posters");
        QTemporaryDir sample;
        QVERIFY(sample.isValid());
        const auto original = sample.filePath(QStringLiteral("sample.mp4"));
        QProcess generate;
        generate.start(ffmpeg, {QStringLiteral("-nostdin"), QStringLiteral("-v"), QStringLiteral("error"),
            QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("color=c=magenta:s=64x48:r=4"), QStringLiteral("-frames:v"), QStringLiteral("1"),
            QStringLiteral("-c:v"), QStringLiteral("mpeg4"), QStringLiteral("-threads"), QStringLiteral("1"), original});
        QVERIFY(generate.waitForFinished(5000));
        QCOMPARE(generate.exitStatus(), QProcess::NormalExit);
        QCOMPARE(generate.exitCode(), 0);
        const QFileInfo fixture(original);
        QVERIFY(fixture.isFile() && fixture.size() > 0);

        std::unique_ptr<FixtureBackend> backend(makeFixture());
        MessageFile video;
        video.id = QStringLiteral("98");
        video.name = QStringLiteral("forwarded.mp4");
        video.size = fixture.size();
        video.downloadUrl = backend->portalOrigin().resolved(QUrl(QStringLiteral("/disk/downloadFile/98/")));
        video.isVideo = true;
        auto &message = backend->messages.first();
        message.files = {video};
        message.forwarded = true;
        message.forwardedAuthor = QStringLiteral("Сотрудник");

        LocalFileOpener player;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(backend->videoRequests.size(), 1);
        QCOMPARE(player.opened.size(), 0);
        const auto [url, path] = backend->videoRequests.first();
        QCOMPARE(url, video.downloadUrl);
        QVERIFY(QFileInfo(path).isAbsolute());
        QCOMPARE(QFileInfo(path).fileName(), QStringLiteral("clip.mp4"));

        const auto findPixel = [&](const std::function<bool(const QColor &)> &match) {
            const auto pixels = history->viewport()->grab().toImage();
            const auto bounds = history->visualItemRect(history->item(0)).intersected(history->viewport()->rect());
            for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
                for (int x = bounds.left(); x <= bounds.right(); ++x) {
                    if (match(pixels.pixelColor(x, y)))
                        return QPoint(x, y);
                }
            }
            return QPoint();
        };
        if (clickWhileDownloading) {
            const auto tile = [&] { return findPixel([](const QColor &color) { return color == QColor("#f6faf8"); }); };
            QTRY_VERIFY(!tile().isNull());
            QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, tile());
            QCOMPARE(backend->videoRequests.size(), 1);
            QCOMPARE(player.opened.size(), 0);
        }
        QVERIFY(QFile::copy(original, path));
        emit backend->messageVideoDownloaded(url, path);
        const auto poster = [&] {
            return findPixel([](const QColor &color) { return color.red() > 220 && color.green() < 35 && color.blue() > 220; });
        };
        QTRY_VERIFY_WITH_TIMEOUT(!poster().isNull(), 10000);
        if (!clickWhileDownloading) {
            QCOMPARE(player.opened.size(), 0);
            QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, poster());
        }
        QTRY_COMPARE(player.opened.size(), 1);
        QCOMPARE(player.opened.first(), QUrl::fromLocalFile(path));
        QCOMPARE(backend->videoRequests.size(), 1);
    }

    void messageGalleryUsesOneBubbleAndCompactClickableQuote()
    {
        QListWidget list;
        auto *delegate = workspace::configureRows(&list, workspace::Row::Message);
        list.resize(650, 660);
        const QColor firstColor("#ce4261");
        const QColor secondColor("#3bad77");
        QPixmap first(160, 92);
        QPixmap second(160, 92);
        first.fill(firstColor);
        second.fill(secondColor);
        const QUrl firstUrl(QStringLiteral("https://example.bitrix24.ru/one.png"));
        const QUrl secondUrl(QStringLiteral("https://example.bitrix24.ru/two.png"));
        delegate->setMessagePreviewSource([&](const QUrl &url) -> const QPixmap * {
            return url == firstUrl ? &first : url == secondUrl ? &second : nullptr;
        });
        QStringList opened;
        delegate->setMessagePhotoHandler([&](const MessageFile &file) { opened.append(file.id); });
        delegate->setReplyNavigation([&](const QString &id) { opened.append(QStringLiteral("reply:") + id); });
        delegate->setMessagePhotoMenuHandler([](const QModelIndex &, const MessageFile &, const QRect &) {});

        Message gallery;
        gallery.id = QStringLiteral("101");
        gallery.author = QStringLiteral("Анна");
        gallery.text = QStringLiteral("[URL=https://example.bitrix24.ru/attachment-note]Описание галереи[/URL]");
        gallery.files = {{QStringLiteral("11"), QStringLiteral("Первая.png"), 3000, {}, firstUrl},
                         {QStringLiteral("12"), QStringLiteral("Вторая.png"), 3200, {}, secondUrl}};
        Message reply;
        reply.id = QStringLiteral("102");
        reply.replyId = gallery.id;
        reply.text = QStringLiteral("Принято");
        QLabel state;
        ui::presentMessages(&list, &state, {gallery, reply});
        list.show();
        QTRY_COMPARE(list.count(), 2);
        QTRY_VERIFY(list.visualItemRect(list.item(1)).isValid());

        const auto grab = list.viewport()->grab();
        const auto capture = qEnvironmentVariable("B24_CHAT_GALLERY_SCREENSHOT");
        if (!capture.isEmpty())
            QVERIFY(grab.save(capture));
        auto pixels = grab.toImage();
        const auto colorBounds = [&](int row, const QColor &color) {
            const auto bounds = list.visualItemRect(list.item(row)).intersected(list.viewport()->rect());
            QRect painted;
            for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
                for (int x = bounds.left(); x <= bounds.right(); ++x) {
                    if (pixels.pixelColor(x, y) == color)
                        painted |= QRect(x, y, 1, 1);
                }
            }
            return painted;
        };
        const auto firstTile = colorBounds(0, firstColor);
        const auto secondTile = colorBounds(0, secondColor);
        QVERIFY(firstTile.isValid() && secondTile.isValid());
        QVERIFY(firstTile.right() < secondTile.left());
        QVERIFY(qAbs(firstTile.top() - secondTile.top()) <= 4);

        const auto original = list.model()->index(0, 0);
        const auto row = list.visualRect(original);
        const auto bubble = delegate->receiptArea(original, row);
        QPoint link;
        for (int y = bubble.top(); y < bubble.bottom() && link.isNull(); ++y) {
            const QPoint candidate(bubble.left() + 22, y);
            if (delegate->anchorAt(original, row, candidate).endsWith(QStringLiteral("/attachment-note")))
                link = candidate;
        }
        QVERIFY(!link.isNull());
        QVERIFY(link.y() > qMax(firstTile.bottom(), secondTile.bottom()));

        const auto quotePreview = colorBounds(1, firstColor);
        QVERIFY(quotePreview.isValid());
        QVERIFY(quotePreview.width() <= 44 && quotePreview.height() <= 44);
        QVERIFY(colorBounds(1, secondColor).isEmpty());
        const auto quoted = list.model()->index(1, 0);
        const auto quoteRow = list.visualRect(quoted);
        QVERIFY(quoteRow.height() < 135);
        QVERIFY(delegate->receiptArea(quoted, quoteRow).width() >= 285);
        QVERIFY(!delegate->showPhotoMenuAt(quoted, quotePreview.center()));

        for (const auto &point : {firstTile.center(), secondTile.center(), quotePreview.center()})
            QTest::mouseClick(list.viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(opened, (QStringList{QStringLiteral("11"), QStringLiteral("12"), QStringLiteral("reply:101")}));

        list.resize(215, 660);
        list.doItemsLayout();
        const auto narrowGrab = list.viewport()->grab();
        const auto narrowCapture = qEnvironmentVariable("B24_CHAT_GALLERY_NARROW_SCREENSHOT");
        if (!narrowCapture.isEmpty())
            QVERIFY(narrowGrab.save(narrowCapture));
        pixels = narrowGrab.toImage();
        const auto firstNarrow = colorBounds(0, firstColor);
        const auto secondNarrow = colorBounds(0, secondColor);
        QVERIFY(firstNarrow.isValid() && secondNarrow.isValid());
        QVERIFY(firstNarrow.bottom() < secondNarrow.top());
        QVERIFY(qAbs(firstNarrow.left() - secondNarrow.left()) <= 4);
        QVERIFY(delegate->receiptArea(quoted, list.visualRect(quoted)).right() < list.viewport()->width());
    }

    void chatAttachmentTooltipsShowEscapedFullNames()
    {
        QListWidget list;
        auto *delegate = workspace::configureRows(&list, workspace::Row::Message);
        list.resize(420, 420);
        const QUrl preview(QStringLiteral("https://example.bitrix24.ru/preview.png"));
        const QColor photoColor("#518faf");
        QPixmap photo(120, 80);
        photo.fill(photoColor);
        delegate->setMessagePreviewSource([&](const QUrl &url) { return url == preview ? &photo : nullptr; });
        delegate->setMessagePhotoMenuHandler([](const QModelIndex &, const MessageFile &, const QRect &) {});

        Message document;
        document.id = QStringLiteral("document");
        const auto docName = QStringLiteral("Подробный отчёт для бухгалтерии за  целый год <b>Q&А</b>.xlsx");
        document.files = {{QStringLiteral("1"), docName, 36152, {}}};
        Message quote;
        quote.id = QStringLiteral("quote");
        quote.replyId = document.id;
        quote.text = QStringLiteral("Получила файл");
        Message picture;
        picture.id = QStringLiteral("picture");
        const auto photoName = QStringLiteral("Очень длинное имя снимка <i>корпус  & этаж</i>.png");
        picture.files = {{QStringLiteral("2"), photoName, 41000, {}, preview}};
        QLabel state;
        ui::presentMessages(&list, &state, {document, quote, picture});
        list.show();
        QTRY_COMPARE(list.count(), 3);
        QTRY_VERIFY(list.visualItemRect(list.item(2)).isValid());
        const auto pixels = list.viewport()->grab().toImage();
        const auto locate = [&](int row, const QColor &color) {
            const auto bounds = list.visualItemRect(list.item(row)).intersected(list.viewport()->rect());
            for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
                for (int x = bounds.left(); x <= bounds.right(); ++x) {
                    if (pixels.pixelColor(x, y) == color)
                        return QPoint(x, y);
                }
            }
            return QPoint();
        };
        const auto hint = [&](const QPoint &point) {
            QHelpEvent event(QEvent::ToolTip, point, list.viewport()->mapToGlobal(point));
            QApplication::sendEvent(list.viewport(), &event);
            QTextDocument text;
            text.setHtml(QToolTip::text());
            return text.toPlainText();
        };
        const auto documentPoint = locate(0, QColor("#f6faf8"));
        const auto quotePoint = locate(1, QColor("#e8f1ee"));
        const auto photoPoint = locate(2, photoColor);
        QVERIFY(!documentPoint.isNull());
        QVERIFY(!quotePoint.isNull());
        QVERIFY(!photoPoint.isNull());
        QCOMPARE(hint(documentPoint), docName);
        QCOMPARE(hint(quotePoint), docName);
        QCOMPARE(hint(photoPoint), photoName);
        QToolTip::hideText();
    }

    void fileCardsSaveToExplicitPathAndReuseQuotedFiles()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdMessageFileDownload = true;
        LocalFileOpener opener;
        const QUrl url(QStringLiteral("https://example.bitrix24.ru/company/personal/file/download/1/"));
        Message fileMessage;
        fileMessage.id = QStringLiteral("file");
        fileMessage.outgoing = true;
        fileMessage.attachments = {QStringLiteral("Смета.xlsx")};
        fileMessage.files = {{QStringLiteral("1"), QStringLiteral("Смета.xlsx"), 36152, url}};
        Message reply;
        reply.id = QStringLiteral("reply");
        reply.replyId = fileMessage.id;
        reply.author = QStringLiteral("Анна");
        reply.text = QStringLiteral("Получила файл");
        backend->messages = {fileMessage, reply};
        backend->taskMessages = backend->messages;
        MainWindow window(backend.get());
        window.resize(960, 650);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 2);
        const auto *item = history->item(0);
        QCOMPARE(item->data(workspace::MessageFiles).value<QVector<MessageFile>>().size(), 1);
        QVERIFY(!item->data(workspace::Detail).toString().contains(QStringLiteral("Смета.xlsx")));
        QCOMPARE(history->item(1)->data(workspace::ReplyFiles).value<QVector<MessageFile>>().size(), 1);
        QCOMPARE(history->item(1)->data(workspace::ReplyPreview).toString(), QStringLiteral("Вложение"));
        const auto capture = qEnvironmentVariable("B24_CHAT_FILE_SCREENSHOT");
        if (!capture.isEmpty()) {
            QCoreApplication::processEvents();
            QVERIFY(window.grab().save(capture));
        }
        const auto row = history->visualItemRect(item);
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(row.left() + 130, row.bottom() - 20));
        QTRY_VERIFY(window.findChild<QFileDialog *>(QStringLiteral("messageFileSaveDialog")));
        auto *dialog = window.findChild<QFileDialog *>(QStringLiteral("messageFileSaveDialog"));
        QCOMPARE(dialog->acceptMode(), QFileDialog::AcceptSave);
        QVERIFY(backend->downloadedPath.isEmpty());
        QTemporaryDir destination;
        QVERIFY(destination.isValid());
        const auto path = destination.filePath(QStringLiteral("confirmed.xlsx"));
        dialog->selectFile(path);
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));
        QTRY_COMPARE(backend->downloadedPath, path);
        QCOMPARE(backend->downloadedUrl, url);

        const auto offerName = QStringLiteral("savedFileOffer");
        const auto oldDialog = QPointer<QFileDialog>(dialog);
        emit backend->messageFileDownloaded(url, destination.filePath(QStringLiteral("unexpected.xlsx")));
        QVERIFY(!window.findChild<QMessageBox *>(offerName));
        emit backend->failed(QStringLiteral("message-file-download"), QStringLiteral("Ошибка фикстуры"));
        QFile saved(path);
        QVERIFY(saved.open(QIODevice::WriteOnly));
        QCOMPARE(saved.write("fixture"), 7);
        saved.close();
        emit backend->messageFileDownloaded(url, path);
        QVERIFY(!window.findChild<QMessageBox *>(offerName));
        QVERIFY(opener.opened.isEmpty());
        QVERIFY(saved.remove());

        QTRY_VERIFY(!oldDialog);
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(row.left() + 130, row.bottom() - 20));
        QTRY_VERIFY(window.findChild<QFileDialog *>(QStringLiteral("messageFileSaveDialog")));
        dialog = window.findChild<QFileDialog *>(QStringLiteral("messageFileSaveDialog"));
        dialog->selectFile(path);
        backend->downloadedPath.clear();
        QVERIFY(QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection));
        QTRY_COMPARE(backend->downloadedPath, path);
        QVERIFY(saved.open(QIODevice::WriteOnly));
        QCOMPARE(saved.write("fixture"), 7);
        saved.close();
        emit backend->messageFileDownloaded(url, path);
        auto *offer = window.findChild<QMessageBox *>(offerName);
        QVERIFY(offer && offer->isVisible());
        QVERIFY(opener.opened.isEmpty());
        auto *open = findAccessible<QPushButton>(offer, QStringLiteral("Открыть"));
        QVERIFY(open);
        open->click();
        QCOMPARE(opener.opened, QList<QUrl>{QUrl::fromLocalFile(path)});

        auto *sections = findAccessible<QListWidget>(&window, QStringLiteral("Разделы"));
        QVERIFY(sections);
        sections->setCurrentRow(1);
        window.openTaskCard(QStringLiteral("501"));
        auto *task = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(task);
        auto *taskHistory = findAccessible<QListWidget>(task, QStringLiteral("Чат карточки"));
        QVERIFY(taskHistory);
        QTRY_COMPARE(taskHistory->count(), 2);
        QCOMPARE(taskHistory->item(1)->data(workspace::ReplyFiles).value<QVector<MessageFile>>().size(), 1);
    }

    void showsExternalLinkCardInBothHistories()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        Message message;
        message.author = QStringLiteral("Анна");
        message.id = QStringLiteral("701");
        message.text = QStringLiteral("[URL=https://example.test/page]Ссылка[/URL]");
        const auto linked = message;
        message.id = QStringLiteral("702");
        message.text = QStringLiteral("Ссылка");
        backend->messages = {linked, message};
        backend->taskMessages = backend->messages;

        MainWindow window(backend.get());
        window.resize(960, 790);
        window.show();
        const auto check = [](QListWidget *history) {
            QVERIFY(history);
            QTRY_COMPARE(history->count(), 2);
            QTRY_VERIFY(history->sizeHintForRow(0) > history->sizeHintForRow(1) + 20);
            const auto row = history->visualItemRect(history->item(0));
            const QPoint preview(row.left() + 65, row.bottom() - 18);
            auto *viewport = history->viewport();
            QVERIFY(viewport->rect().contains(preview));
            QMouseEvent move(QEvent::MouseMove, preview, viewport->mapToGlobal(preview),
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(viewport, &move);
            QCOMPARE(viewport->cursor().shape(), Qt::PointingHandCursor);
        };
        check(findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата")));
        window.openTaskCard(QStringLiteral("501"));
        auto *task = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(task);
        check(findAccessible<QListWidget>(task, QStringLiteral("Чат карточки")));
    }

    void fetchesOnlyMissingPublicLinksFromTheVisibleOutgoingRowOrAnExplicitAction()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        backend->chats.first().id = QStringLiteral("chat91");
        backend->holdExternalLinkPreviews = true;
        const auto make = [](const QString &id, const QUrl &url, bool outgoing) {
            Message message;
            message.id = id;
            message.outgoing = outgoing;
            message.author = outgoing ? QStringLiteral("Вы") : QStringLiteral("Анна");
            message.text = url.toString();
            return message;
        };
        const QUrl portalRich(QStringLiteral("https://public-preview.example.com/rich"));
        const QUrl outgoing(QStringLiteral("https://public-preview.example.com/first"));
        const QUrl unavailable(QStringLiteral("https://public-preview.example.com/unavailable"));
        const QUrl incoming(QStringLiteral("https://public-preview.example.com/incoming"));
        const QUrl forwarded(QStringLiteral("https://public-preview.example.com/forwarded"));
        auto duplicate = outgoing;
        duplicate.setFragment(QStringLiteral("same-page"));
        auto forwardedMessage = make(QStringLiteral("716"), forwarded, true);
        forwardedMessage.forwarded = true;
        forwardedMessage.forwardedAuthor = QStringLiteral("Другой автор");
        backend->messages = {make(QStringLiteral("711"), portalRich, true), make(QStringLiteral("712"), outgoing, true),
            make(QStringLiteral("713"), unavailable, true), make(QStringLiteral("714"), incoming, false),
            make(QStringLiteral("715"), duplicate, true), forwardedMessage};
        QVector<QPair<QUrl, ui::PublicLinkPreviewFetcher::Completion>> requests;
        MainWindow window(backend.get(), false, nullptr, [&](const QUrl &url, auto complete) {
            requests.append({url, std::move(complete)});
            return true;
        });
        window.resize(1000, 780);
        window.show();
        QApplication::setActiveWindow(&window);
        emit backend->connectionChanged(true);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 6);
        QTRY_COMPARE(backend->externalLinkPreviewRequests.size(), 1);
        QVERIFY(requests.isEmpty());
        const auto showMessage = [history](int row) {
            history->scrollToItem(history->item(row), QAbstractItemView::PositionAtCenter);
            QTRY_VERIFY(history->viewport()->rect().contains(history->visualItemRect(history->item(row)).center()));
        };
        showMessage(1);
        ChatAsset portal;
        portal.messageId = QStringLiteral("711");
        portal.link = portalRich;
        portal.name = QStringLiteral("Предпросмотр портала");
        portal.description = QStringLiteral("Данные портала");
        auto deliverPortal = std::move(backend->externalLinkPreviewRequests.first().second);
        deliverPortal({portal});
        QTRY_COMPARE(requests.size(), 1);
        QCOMPARE(requests.constFirst().first, outgoing);
        const int before = history->sizeHintForRow(1);
        auto deliverPublic = std::move(requests.first().second);
        QImage thumbnail(116, 100, QImage::Format_ARGB32_Premultiplied);
        const QColor thumbnailColor("#994064");
        thumbnail.fill(thumbnailColor);
        {
            QPainter painter(&thumbnail);
            painter.fillRect(QRect(24, 20, 66, 48), QColor("#fbe2ed"));
            painter.fillRect(QRect(32, 30, 50, 5), thumbnailColor);
            painter.fillRect(QRect(32, 44, 22, 15), thumbnailColor);
        }
        const auto thumbnailDrawn = [thumbnailColor](QListWidget *list, int row) {
            const auto image = list->viewport()->grab().toImage();
            const auto bounds = list->visualItemRect(list->item(row)).intersected(image.rect());
            int pixels = 0;
            for (int y = bounds.top(); y <= bounds.bottom(); ++y)
                for (int x = bounds.left(); x <= bounds.right(); ++x)
                    pixels += image.pixelColor(x, y) == thumbnailColor ? 1 : 0;
            return pixels > 100;
        };
        deliverPublic(ui::PublicLinkMetadata{QStringLiteral("Заголовок сайта"), QStringLiteral("Описание сайта"), thumbnail});
        QTRY_VERIFY(history->sizeHintForRow(1) > before);
        showMessage(1);
        QTRY_VERIFY(thumbnailDrawn(history, 1));
        const auto screenshot = qEnvironmentVariable("B24_PUBLIC_PREVIEW_IMAGE_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(history->viewport()->grab().save(screenshot));
        QVERIFY(backend->imageRequests.isEmpty());
        showMessage(2);
        QTRY_COMPARE(requests.size(), 2);
        QCOMPARE(requests.last().first, unavailable);
        auto deliverUnavailable = std::move(requests.last().second);
        deliverUnavailable(std::nullopt);
        emit backend->chatMessagesLoaded(QStringLiteral("chat91"), backend->messages);
        QCoreApplication::sendPostedEvents(&window, QEvent::MetaCall);
        QCOMPARE(requests.size(), 2);

        history->setCurrentRow(3);
        showMessage(3);
        const auto point = history->visualItemRect(history->item(3)).center();
        QContextMenuEvent event(QContextMenuEvent::Keyboard, point, history->viewport()->mapToGlobal(point));
        QApplication::sendEvent(history->viewport(), &event);
        auto *menu = visibleMessageMenu();
        QVERIFY(menu);
        QAction *preview = nullptr;
        for (auto *action : menu->actions())
            if (action->text() == QStringLiteral("Загрузить предпросмотр ссылки"))
                preview = action;
        QVERIFY(preview && preview->isEnabled());
        menu->setActiveAction(preview);
        QTest::keyClick(menu, Qt::Key_Return);
        QTRY_COMPARE(requests.size(), 3);
        QCOMPARE(requests.last().first, incoming);
        auto deliverIncoming = std::move(requests.last().second);
        deliverIncoming(ui::PublicLinkMetadata{{}, {}, thumbnail});
        QTRY_VERIFY(thumbnailDrawn(history, 3));
        QVERIFY(backend->imageRequests.isEmpty());

        backend->taskMessages = {make(QStringLiteral("721"), duplicate, true)};
        window.openTaskCard(QStringLiteral("501"));
        auto *task = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(task);
        auto *taskHistory = findAccessible<QListWidget>(task, QStringLiteral("Чат карточки"));
        QVERIFY(taskHistory);
        QTRY_COMPARE(taskHistory->count(), 1);
        QTRY_VERIFY(thumbnailDrawn(taskHistory, 0));
        QCOMPARE(requests.size(), 3);
    }

    void drawsAuthorizedLinkAvatar()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdLinkPreviews = true;
        const QUrl avatar(QStringLiteral("https://example.bitrix24.ru/upload/preview-person.png"));
        const QColor avatarColor("#8d3c58");
        const QColor projectColor("#286f62");
        QByteArray bytes;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QImage photo(64, 64, QImage::Format_RGB32);
        photo.fill(avatarColor);
        {
            QPainter painter(&photo);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setBrush(QColor("#f7d9cd"));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(QRect(22, 11, 20, 21));
            painter.drawEllipse(QRect(13, 37, 38, 33));
        }
        QVERIFY(photo.save(&buffer, "PNG"));
        backend->avatarImages.insert(avatar, bytes);
        Message task;
        task.author = QStringLiteral("Поддержка");
        task.id = QStringLiteral("701");
        task.text = QStringLiteral("Создана [URL=/tasks/task/view/42/]задача[/URL]");
        auto person = task;
        person.id = QStringLiteral("702");
        person.text = QStringLiteral("[URL=/company/personal/user/19/]Анна Волкова[/URL]");
        backend->messages = {task, person};

        MainWindow window(backend.get());
        window.resize(950, 760);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 2);
        QVERIFY(!backend->avatarRequests.contains(avatar));
        for (auto &[url, complete] : backend->linkPreviewRequests) {
            if (url.path().contains(QStringLiteral("/tasks/"))) {
                MessageLinkPreview preview{QStringLiteral("Восстановить принтер и кнопку печати"),
                    {QStringLiteral("Статус · Ждёт выполнения"), QStringLiteral("Исполнитель · Анна Волкова"),
                     QStringLiteral("Проект · IT разработка")}, MessageLinkPreviewKind::Task};
                Project project{QStringLiteral("7"), QStringLiteral("IT разработка"), {}};
                project.avatarType = QStringLiteral("folder");
                project.avatarColor = projectColor.name();
                preview.project = std::move(project);
                complete(std::move(preview));
            } else {
                complete({QStringLiteral("Анна Волкова"), {QStringLiteral("Инженер поддержки")},
                          MessageLinkPreviewKind::Person, avatar});
            }
        }
        QTRY_COMPARE(backend->avatarRequests.count(avatar), 1);
        const auto image = history->viewport()->grab().toImage();
        const auto pixelsInRow = [&image, history](int index, QColor color) {
            const auto row = history->visualItemRect(history->item(index)).intersected(image.rect());
            int pixels = 0;
            for (int y = row.top(); y <= row.bottom(); ++y)
                for (int x = row.left(); x <= row.right(); ++x)
                    pixels += image.pixelColor(x, y) == color ? 1 : 0;
            return pixels;
        };
        QVERIFY2(pixelsInRow(0, projectColor) > 60, "The task should draw its project's configured icon");
        QVERIFY2(pixelsInRow(1, avatarColor) > 60, "The authorized profile picture should be drawn inside the link card");
        const auto screenshot = qEnvironmentVariable("B24_LINK_CARD_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(history->viewport()->grab().save(screenshot));
    }

    void rechecksPortalLinkPermissionWhenSwitchingHistories()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdLinkPreviews = true;
        Message message;
        message.author = QStringLiteral("Анна");
        message.id = QStringLiteral("701");
        message.text = QStringLiteral("[URL=/tasks/task/view/42/]Задача[/URL]");
        const auto linked = message;
        message.id = QStringLiteral("702");
        message.text = QStringLiteral("Задача");
        backend->messages = {linked, message};
        backend->taskMessages = backend->messages;

        MainWindow window(backend.get());
        window.resize(960, 790);
        window.show();
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 1);
        window.openTaskCard(QStringLiteral("501"));
        auto *task = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(task);
        auto *history = findAccessible<QListWidget>(task, QStringLiteral("Чат карточки"));
        QVERIFY(history);
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 2);
        QCOMPARE(backend->linkPreviewRequests.at(0).first, backend->linkPreviewRequests.at(1).first);
        auto earlier = std::move(backend->linkPreviewRequests[0].second);
        earlier({QStringLiteral("Доступ в прежнем диалоге"), {}});
        QCoreApplication::processEvents();
        QCOMPARE(history->sizeHintForRow(0), history->sizeHintForRow(1));
        auto current = std::move(backend->linkPreviewRequests[1].second);
        current({});
        QCoreApplication::processEvents();
        QCOMPARE(history->sizeHintForRow(0), history->sizeHintForRow(1));
        QCOMPARE(backend->linkPreviewRequests.size(), 2);
        emit backend->taskUpdated(QStringLiteral("42"));
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 3);
        emit backend->taskLinkPreviewInvalidated(QStringLiteral("701"));
        QCoreApplication::processEvents();
        QCOMPARE(backend->linkPreviewRequests.size(), 3);
        emit backend->taskLinkPreviewInvalidated(QStringLiteral("42"));
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 4);
    }

    void hidesOnlyTheCurrentTaskPreviewInBothTaskHistories()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdLinkPreviews = true;
        const auto self = QStringLiteral("/tasks/task/view/501/");
        const auto moved = QStringLiteral("https://example.bitrix24.ru/workgroups/group/70/tasks/task/view/501/?from=chat");
        const auto other = QStringLiteral("/tasks/task/view/502/");
        const auto external = QStringLiteral("https://elsewhere.example/tasks/task/view/501/");
        const auto author = QStringLiteral("Анна");
        const auto caption = QStringLiteral("Ссылка на задачу");
        backend->taskMessages.clear();
        for (const auto &url : {self, moved, other, external, QString()}) {
            Message message;
            message.id = QString::number(700 + backend->taskMessages.size());
            message.author = author;
            message.text = url.isEmpty() ? caption : QStringLiteral("[URL=%1]%2[/URL]").arg(url, caption);
            backend->taskMessages.append(message);
        }
        backend->messages = backend->taskMessages;
        Chat taskChat{QStringLiteral("task-dialog"), QStringLiteral("Чат задачи"), QStringLiteral("tasksTask")};
        taskChat.taskChat = true;
        taskChat.taskId = QStringLiteral("501");
        backend->taskChats = {taskChat};

        MainWindow window(backend.get());
        window.resize(1020, 850);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        auto *tabs = window.findChild<QTabBar *>(QStringLiteral("chatScopeTabs"));
        QVERIFY(history && tabs);
        const auto complete = [&backend] {
            for (auto &request : backend->linkPreviewRequests) {
                auto completion = std::exchange(request.second, {});
                if (completion)
                    completion({QStringLiteral("Карточка задачи"), {QStringLiteral("Описание")}, MessageLinkPreviewKind::Task});
            }
            backend->linkPreviewRequests.clear();
        };
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 3);
        complete();
        QTRY_VERIFY(history->sizeHintForRow(0) > history->sizeHintForRow(4));
        tabs->setCurrentIndex(1);
        QTRY_VERIFY(!backend->linkPreviewRequests.isEmpty());
        QCOMPARE(backend->linkPreviewRequests.size(), 1);
        QCOMPARE(backend->linkPreviewRequests.first().first.path(), other);
        complete();
        QTRY_COMPARE(history->sizeHintForRow(0), history->sizeHintForRow(4));
        QCOMPARE(history->sizeHintForRow(1), history->sizeHintForRow(4));
        QVERIFY(history->sizeHintForRow(2) > history->sizeHintForRow(4));
        QVERIFY(history->sizeHintForRow(3) > history->sizeHintForRow(4));

        // Scope metadata can arrive on the current chat without resetting its existing cache.
        backend->taskChats[0].taskId.clear();
        emit backend->taskChatsLoaded(backend->taskChats);
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 2);
        complete();
        QTRY_VERIFY(history->sizeHintForRow(0) > history->sizeHintForRow(4));
        backend->taskChats[0].taskId = taskChat.taskId;
        emit backend->taskChatsLoaded(backend->taskChats);
        QTRY_COMPARE(history->sizeHintForRow(0), history->sizeHintForRow(4));
        QCOMPARE(history->sizeHintForRow(1), history->sizeHintForRow(4));
        QVERIFY(backend->linkPreviewRequests.isEmpty());

        auto *delegate = static_cast<workspace::RowPainter *>(history->itemDelegate());
        const auto index = history->model()->index(0, 0);
        const auto row = history->visualItemRect(history->item(0));
        const auto bubble = delegate->receiptArea(index, row);
        std::optional<QPoint> anchor;
        for (int y = bubble.top(); y <= bubble.bottom() && !anchor; y += 2) {
            for (int x = bubble.left(); x <= bubble.right(); x += 2) {
                if (delegate->anchorAt(index, row, QPoint(x, y)) == self) {
                    anchor = QPoint(x, y);
                    break;
                }
            }
        }
        QVERIFY(anchor);
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, *anchor);
        auto *card = dynamic_cast<ui::TaskCardDialog *>(
            window.findChild<QDialog *>(QStringLiteral("taskCardDialog")));
        QVERIFY(card);
        QCOMPARE(card->taskId(), taskChat.taskId);
        auto *cardHistory = findAccessible<QListWidget>(card, QStringLiteral("Чат карточки"));
        QVERIFY(cardHistory);
        QTRY_VERIFY(!backend->linkPreviewRequests.isEmpty());
        QCOMPARE(backend->linkPreviewRequests.size(), 1);
        QCOMPARE(backend->linkPreviewRequests.first().first.path(), other);
        complete();
        QTRY_COMPARE(cardHistory->sizeHintForRow(0), cardHistory->sizeHintForRow(4));
        QCOMPARE(cardHistory->sizeHintForRow(1), cardHistory->sizeHintForRow(4));
        QVERIFY(cardHistory->sizeHintForRow(2) > cardHistory->sizeHintForRow(4));
        QVERIFY(cardHistory->sizeHintForRow(3) > cardHistory->sizeHintForRow(4));
    }

    void refreshesOnlyTheChangedTaskLinkPreviews()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdLinkPreviews = true;
        const auto cached = QStringLiteral("/tasks/task/view/500/");
        const auto alias = QStringLiteral("/workgroups/group/7/tasks/task/view/42/");
        const auto changed = QStringLiteral("/tasks/task/view/42/");
        const auto pending = QStringLiteral("/company/personal/user/19/");
        backend->messages.clear();
        for (const auto &path : {cached, alias, changed, pending}) {
            Message message;
            message.id = QString::number(701 + backend->messages.size());
            message.author = QStringLiteral("Анна");
            message.text = QStringLiteral("[URL=%1]Ссылка[/URL]").arg(path);
            backend->messages.append(std::move(message));
        }

        MainWindow window(backend.get());
        window.resize(960, 790);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        const auto count = [&backend](const QString &path) {
            return std::count_if(backend->linkPreviewRequests.cbegin(), backend->linkPreviewRequests.cend(),
                                 [&path](const auto &entry) { return entry.first.path() == path; });
        };
        const auto takeCallback = [&backend](const QString &path) -> Backend::LinkPreviewHandler {
            for (auto &entry : backend->linkPreviewRequests)
                if (entry.first.path() == path && entry.second)
                    return std::exchange(entry.second, {});
            return {};
        };
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 3);
        auto completeCached = takeCallback(cached);
        auto completeAlias = takeCallback(alias);
        QVERIFY(completeCached && completeAlias);
        completeCached({QStringLiteral("Other task"), {QStringLiteral("Known project")}});
        completeAlias({QStringLiteral("Old alias"), {}});
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 4);
        auto oldChanged = takeCallback(changed);
        auto completePending = takeCallback(pending);
        QVERIFY(oldChanged && completePending);
        const int cachedHeight = history->sizeHintForRow(0);
        const int pendingHeight = history->sizeHintForRow(3);

        emit backend->taskLinkPreviewInvalidated(QStringLiteral("42"));
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 5);
        QCOMPARE(count(cached), 1);
        QCOMPARE(history->sizeHintForRow(0), cachedHeight);
        completePending({QStringLiteral("Other profile"), {QStringLiteral("Authorized details")}});
        QTRY_COMPARE(backend->linkPreviewRequests.size(), 6);
        QCOMPARE(count(pending), 1);
        QTRY_VERIFY(history->sizeHintForRow(3) > pendingHeight);
        QCOMPARE(count(alias), 2);
        QCOMPARE(count(changed), 2);
        auto newChanged = takeCallback(changed);
        auto newAlias = takeCallback(alias);
        QVERIFY(newChanged && newAlias);
        newChanged({QStringLiteral("New project"), {QStringLiteral("Current details")}});
        newAlias({QStringLiteral("New project alias"), {}});
        const int updatedHeight = history->sizeHintForRow(2);
        oldChanged({QStringLiteral("Outdated project"), {}});
        QCoreApplication::processEvents();
        QCOMPARE(history->sizeHintForRow(2), updatedHeight);
        QCOMPARE(backend->linkPreviewRequests.size(), 6);
    }

    void rendersExternalMetadataOnlyForTheMatchingHistoryMessage()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdExternalLinkPreviews = true;
        const QUrl link(QStringLiteral("https://example.test/article"));
        const QUrl internal(QStringLiteral("https://example.bitrix24.ru/unknown"));
        const QUrl thumbnail(QStringLiteral("https://example.bitrix24.ru/upload/link-preview.png"));
        const QColor color("#83326d");
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QImage photo(100, 70, QImage::Format_RGB32);
        photo.fill(color);
        QVERIFY(photo.save(&buffer, "PNG"));
        backend->messageImages.insert(thumbnail, png);
        const auto message = [](const QString &id, const QUrl &url) {
            Message value;
            value.id = id;
            value.author = QStringLiteral("Анна");
            value.text = QStringLiteral("[URL=%1]Ссылка[/URL]").arg(url.toString());
            return value;
        };
        backend->messages = {message(QStringLiteral("701"), link)};
        backend->taskMessages = {message(QStringLiteral("701"), link), message(QStringLiteral("702"), link),
                                 message(QStringLiteral("703"), internal)};

        MainWindow window(backend.get());
        window.resize(960, 790);
        window.show();
        QTRY_COMPARE(backend->externalLinkPreviewRequests.size(), 1);
        QVERIFY(!backend->externalLinkPreviewRequests.first().first.second);
        window.openTaskCard(QStringLiteral("501"));
        auto *task = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(task);
        auto *history = findAccessible<QListWidget>(task, QStringLiteral("Чат карточки"));
        QVERIFY(history);
        QTRY_COMPARE(history->count(), 3);
        const auto fallbackHeight = history->sizeHintForRow(0);
        const auto internalHeight = history->sizeHintForRow(2);
        ChatAsset asset;
        asset.messageId = QStringLiteral("701");
        asset.link = link;
        asset.name = QStringLiteral("Заголовок страницы");
        asset.description = QStringLiteral("Описание страницы");
        asset.thumbnailUrl = thumbnail;
        auto stale = std::exchange(backend->externalLinkPreviewRequests[0].second, {});
        stale({asset});
        QTRY_COMPARE(backend->externalLinkPreviewRequests.size(), 2);
        QVERIFY(backend->externalLinkPreviewRequests.last().first.second);
        QCOMPARE(history->sizeHintForRow(0), fallbackHeight);
        QVERIFY(backend->imageRequests.isEmpty());
        auto intranet = asset;
        intranet.messageId = QStringLiteral("703");
        intranet.link = internal;
        auto complete = std::exchange(backend->externalLinkPreviewRequests[1].second, {});
        complete({asset, intranet});
        QTRY_COMPARE(backend->imageRequests.count(thumbnail), 1);
        QTRY_VERIFY(history->sizeHintForRow(0) > fallbackHeight);
        QCOMPARE(history->sizeHintForRow(1), fallbackHeight);
        QCOMPARE(history->sizeHintForRow(2), internalHeight);
        const auto thumbnailDrawn = [history, color] {
            const auto image = history->viewport()->grab().toImage();
            const auto bounds = history->visualItemRect(history->item(0)).intersected(image.rect());
            int pixels = 0;
            for (int y = bounds.top(); y <= bounds.bottom(); ++y)
                for (int x = bounds.left(); x <= bounds.right(); ++x)
                    pixels += image.pixelColor(x, y) == color ? 1 : 0;
            return pixels > 100;
        };
        QTRY_VERIFY(thumbnailDrawn());
        const auto screenshot = qEnvironmentVariable("B24_EXTERNAL_RICH_SCREENSHOT");
        if (!screenshot.isEmpty())
            QVERIFY(history->viewport()->grab().save(screenshot));
        backend->taskMessages.append(message(QStringLiteral("704"), link));
        emit backend->taskMessagesLoaded(QStringLiteral("501"), backend->taskMessages);
        QTRY_COMPARE(backend->externalLinkPreviewRequests.size(), 3);
    }

    void inlineChatPreviewAnimatesUntilTerminalResult_data()
    {
        QTest::addColumn<bool>("fallback");
        QTest::addColumn<bool>("succeeds");
        QTest::newRow("preferred succeeds") << false << true;
        QTest::newRow("preferred fails") << false << false;
        QTest::newRow("original succeeds") << true << true;
    }

    void inlineChatPreviewAnimatesUntilTerminalResult()
    {
        QFETCH(bool, fallback);
        QFETCH(bool, succeeds);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdMessageImages = true;
        MessageFile file;
        file.id = QStringLiteral("800");
        file.name = QStringLiteral("image (800).png");
        file.isImage = true;
        file.size = 8611;
        file.downloadUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/disk/downloadFile/800/"));
        file.previewUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/disk/showFile/800/"));
        if (fallback)
            file.originalPreviewUrl = file.downloadUrl;
        backend->messages.first().files = {file};

        MainWindow window(backend.get());
        window.resize(900, 600);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(backend->imageRequests.count(file.previewUrl), 1);
        if (fallback) {
            emit backend->messageImageLoaded(file.previewUrl, {});
            QTRY_COMPARE_WITH_TIMEOUT(backend->imageRequests.count(file.downloadUrl), 1, 800);
        }

        const auto rowImage = [history] {
            const auto row = history->visualItemRect(history->item(0));
            return history->viewport()->grab(row).toImage();
        };
        const auto firstFrame = rowImage();
        QVERIFY(!firstFrame.isNull());
        auto &animationTimer = testClock(*ui::LoadingClock::shared());
        QVERIFY(animationTimer.isActive());
        fireTimer(animationTimer);
        QVERIFY(rowImage() != firstFrame);

        QByteArray bytes;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        const QColor thumbnailColor("#749fb1");
        QImage image(634, 60, QImage::Format_RGB32);
        image.fill(thumbnailColor);
        if (succeeds)
            QVERIFY(image.save(&buffer, "PNG"));
        emit backend->messageImageLoaded(fallback ? file.downloadUrl : file.previewUrl, bytes);
        QCoreApplication::processEvents();
        const auto finalFrame = rowImage();
        bool hasImage = false;
        for (int y = 0; y < finalFrame.height() && !hasImage; ++y)
            for (int x = 0; x < finalFrame.width() && !hasImage; ++x)
                hasImage = finalFrame.pixelColor(x, y) == thumbnailColor;
        QCOMPARE(hasImage, succeeds);
        QVERIFY(!ui::LoadingClock::shared()->isRunning());
        QCOMPARE(rowImage(), finalFrame);
        QCOMPARE(backend->imageRequests.size(), fallback ? 2 : 1);
    }

    void chatPreviewRequestsRestartOnlyAfterAnAccountChange()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->holdMessageImages = true;
        MessageFile file;
        file.id = QStringLiteral("800");
        file.name = QStringLiteral("image.png");
        file.isImage = true;
        file.previewUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/disk/showFile/800/"));
        auto other = file;
        other.id = QStringLiteral("801");
        other.previewUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/disk/showFile/801/"));
        backend->messages.first().files = {file, other};

        MainWindow window(backend.get());
        window.resize(900, 600);
        window.show();
        QTRY_COMPARE(backend->imageRequests.size(), 2);

        backend->accountScope = QStringLiteral("fixture/preview-account-one");
        emit backend->accountScopeChanged();
        QCoreApplication::processEvents();
        QCOMPARE(backend->imageRequests.size(), 2);

        // Account changes cancel raster requests without delivering a completion.
        backend->accountScope = QStringLiteral("fixture/preview-account-two");
        emit backend->accountScopeChanged();
        emit backend->recentChatsLoaded(backend->chats);
        QTRY_COMPARE_WITH_TIMEOUT(backend->imageRequests.count(file.previewUrl), 2, 800);
        QCOMPARE(backend->imageRequests.count(other.previewUrl), 2);
    }

    void imageContextMenuUsesTheClickedPhoto_data()
    {
        QTest::addColumn<bool>("viewer");
        QTest::addColumn<bool>("hasOriginal");
        QTest::addColumn<bool>("brokenPreview");
        QTest::newRow("inline original") << false << true << false;
        QTest::newRow("original viewer") << true << true << false;
        QTest::newRow("inline preview only") << false << false << false;
        QTest::newRow("preview only viewer") << true << false << false;
        QTest::newRow("inline original after broken preview") << false << true << true;
    }

    void imageContextMenuUsesTheClickedPhoto()
    {
        QFETCH(bool, viewer);
        QFETCH(bool, hasOriginal);
        QFETCH(bool, brokenPreview);
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        auto &message = backend->messages.first();
        message.id = QStringLiteral("92");
        const QColor previewColor("#518faf");
        const QColor viewerColor("#294563");
        const QSize previewSize(120, 75);
        const QSize originalSize(600, 375);
        for (const int id : {1, 2}) {
            MessageFile file;
            file.id = QString::number(id);
            file.name = QStringLiteral("Фото %1.jpg").arg(id);
            file.previewUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/preview/%1").arg(id));
            if (id == 1 || hasOriginal)
                file.downloadUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/download/%1").arg(id));
            file.originalPreviewUrl = file.downloadUrl;
            file.previewSize = previewSize;
            file.isImage = true;
            message.files.append(file);
            QByteArray bytes;
            QBuffer buffer(&bytes);
            QVERIFY(buffer.open(QIODevice::WriteOnly));
            QImage image(previewSize, QImage::Format_RGB32);
            image.fill(id == 1 ? QColor("#87593b") : previewColor);
            QVERIFY(image.save(&buffer, "PNG"));
            if (id == 2 && brokenPreview)
                backend->messageImages.insert(file.previewUrl, QByteArray::fromHex("89504e470d0a1a0a0000000d49484452"));
            else
                backend->messageImages.insert(file.previewUrl, bytes);
            if (!file.downloadUrl.isEmpty())
                backend->messageImages.insert(file.downloadUrl, bytes);
        }
        const auto photo = message.files.last();
        MainWindow window(backend.get());
        window.resize(1000, 760);
        window.show();
        QApplication::setActiveWindow(&window);
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(backend->imageRequests.count(photo.previewUrl), 1);
        if (brokenPreview)
            QTRY_COMPARE_WITH_TIMEOUT(backend->imageRequests.count(photo.downloadUrl), 1, 1200);
        const auto pointInPreview = [&] {
            const auto pixels = history->viewport()->grab().toImage();
            for (int y = 0; y < pixels.height(); ++y)
                for (int x = 0; x < pixels.width(); ++x)
                    if (pixels.pixelColor(x, y) == previewColor)
                        return QPoint(x, y);
            return QPoint();
        };
        QTRY_VERIFY(!pointInPreview().isNull());
        QCOMPARE(backend->imageRequests.count(photo.downloadUrl), brokenPreview ? 1 : 0);
        QWidget *target = history->viewport();
        auto point = pointInPreview();
        const auto openContext = [](QWidget *widget, const QPoint &pos) {
            QContextMenuEvent event(QContextMenuEvent::Mouse, pos, widget->mapToGlobal(pos));
            QCoreApplication::sendEvent(widget, &event);
        };
        if (viewer) {
            QTest::mouseClick(target, Qt::LeftButton, Qt::NoModifier, point);
            auto *dialog = window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer"));
            QVERIFY(dialog);
            if (hasOriginal) {
                auto *state = findAccessible<QLabel>(dialog, QStringLiteral("Состояние загрузки фотографии"));
                QVERIFY(state && state->isVisible());
                openContext(state, state->rect().center());
                QTRY_VERIFY(visibleMessageMenu(QStringLiteral("imageContextMenu")));
                auto *loadingMenu = visibleMessageMenu(QStringLiteral("imageContextMenu"));
                QVERIFY(loadingMenu->actions().first()->isEnabled());
                QVERIFY(!loadingMenu->actions().last()->isEnabled());
                loadingMenu->close();
                QCOMPARE(backend->photoUrl, photo.downloadUrl);
                QImage original(originalSize, QImage::Format_RGB32);
                original.fill(viewerColor);
                QVERIFY(original.save(backend->photoPath, "PNG"));
                emit backend->messageFileDownloaded(backend->photoUrl, backend->photoPath);
            }
            target = dialog->findChild<QWidget *>(QStringLiteral("chatPhotoCanvas"));
            QTRY_VERIFY(target && target->isVisible());
            auto *zoom = findAccessible<QPushButton>(dialog, QStringLiteral("Увеличить фотографию"));
            QVERIFY(zoom && zoom->isEnabled());
            zoom->click();
            point = target->rect().center();
        } else if (hasOriginal) {
            const auto row = history->visualItemRect(history->item(0));
            openContext(target, QPoint(row.right() - 5, row.top() + 12));
            QTRY_VERIFY(visibleMessageMenu());
            visibleMessageMenu()->close();
        }
        auto *clipboard = QGuiApplication::clipboard();
        clipboard->setText(QStringLiteral("clipboard sentinel"));
        openContext(target, point);
        QTRY_VERIFY(visibleMessageMenu(QStringLiteral("imageContextMenu")));
        auto *menu = visibleMessageMenu(QStringLiteral("imageContextMenu"));
        QCOMPARE(menu->actions().size(), 2);
        QCOMPARE(menu->actions().first()->text(), QStringLiteral("Скачать изображение"));
        QCOMPARE(menu->actions().last()->text(), QStringLiteral("Скопировать изображение"));
        QVERIFY(menu->actions().last()->isEnabled());
        menu->actions().last()->trigger();
        QTRY_VERIFY(!visibleMessageMenu(QStringLiteral("imageContextMenu")));
        const auto pixels = clipboard->image();
        const bool showOriginal = viewer && hasOriginal;
        QCOMPARE(pixels.size(), showOriginal ? originalSize : previewSize);
        QCOMPARE(pixels.pixelColor(3, 3), showOriginal ? viewerColor : previewColor);
        openContext(target, point);
        QTRY_VERIFY(visibleMessageMenu(QStringLiteral("imageContextMenu")));
        menu = visibleMessageMenu(QStringLiteral("imageContextMenu"));
        QVERIFY(menu->actions().first()->isEnabled());
        menu->actions().first()->trigger();
        auto *save = window.findChild<QFileDialog *>(QStringLiteral("messageFileSaveDialog"));
        QTRY_VERIFY(save && save->isVisible());
        QCOMPARE(QFileInfo(save->selectedFiles().value(0)).fileName(),
                 hasOriginal ? photo.name : QStringLiteral("Фото 2.png"));
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
#ifdef Q_OS_UNIX
        if (!hasOriginal) {
            QFile protectedFile(folder.filePath(QStringLiteral("protected.txt")));
            QVERIFY(protectedFile.open(QIODevice::WriteOnly));
            QCOMPARE(protectedFile.write("keep this file"), qint64(14));
            protectedFile.close();
            const auto link = folder.filePath(QStringLiteral("symlink.png"));
            QVERIFY(QFile::link(protectedFile.fileName(), link));
            QVERIFY(QFileInfo(link).isSymLink());
            QVERIFY(QMetaObject::invokeMethod(save, "fileSelected", Qt::DirectConnection, Q_ARG(QString, link)));
            QVERIFY(QFileInfo(link).isSymLink());
            QVERIFY(protectedFile.open(QIODevice::ReadOnly));
            QCOMPARE(protectedFile.readAll(), QByteArray("keep this file"));
        }
#endif
        const auto path = folder.filePath(hasOriginal ? QStringLiteral("original.jpg") : QStringLiteral("preview.png"));
        save->selectFile(path);
        QVERIFY(QMetaObject::invokeMethod(save, "accept"));
        if (hasOriginal) {
            QTRY_COMPARE(backend->downloadedUrl, photo.downloadUrl);
            QCOMPARE(backend->downloadedPath, path);
        } else {
            QTRY_VERIFY(QFileInfo::exists(path));
            QVERIFY(backend->downloadedUrl.isEmpty());
            const QImage saved(path);
            QCOMPARE(saved.size(), previewSize);
            QCOMPARE(saved.pixelColor(3, 3), previewColor);
        }
    }

    void chatPreviewsLoadOnlyNearVisibleRowsAndShowInsideQuotes()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        const QUrl visible(QStringLiteral("https://example.bitrix24.ru/preview/2"));
        const QUrl distant(QStringLiteral("https://example.bitrix24.ru/preview/1"));
        const QUrl unavailable(QStringLiteral("https://example.bitrix24.ru/preview/3"));
        const QUrl omittedQuotePreview(QStringLiteral("https://example.bitrix24.ru/preview/4"));
        const QColor photoColor("#b85038");
        QByteArray photoBytes;
        {
            QBuffer buffer(&photoBytes);
            QVERIFY(buffer.open(QIODevice::WriteOnly));
            QImage photo(240, 145, QImage::Format_ARGB32);
            photo.fill(photoColor);
            QVERIFY(photo.save(&buffer, "PNG"));
        }
        backend->messageImages.insert(visible, photoBytes);
        backend->messageImages.insert(distant, photoBytes);

        auto makeFile = [](QString id, const QUrl &preview) {
            MessageFile file;
            file.id = id;
            file.name = QStringLiteral("Образец %1.png").arg(id);
            file.size = 3500;
            file.downloadUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/download/%1").arg(id));
            file.previewUrl = preview;
            file.previewSize = QSize(240, 145);
            return file;
        };
        auto makeMessage = [](QString id, QString text, bool outgoing = false) {
            Message message;
            message.id = id;
            message.author = outgoing ? QStringLiteral("Вы") : QStringLiteral("Анна");
            message.text = text;
            message.outgoing = outgoing;
            return message;
        };
        auto top = makeMessage(QStringLiteral("1"), QStringLiteral("Старая фотография"));
        top.files = {makeFile(QStringLiteral("1"), distant)};
        backend->messages = {top};
        for (int row = 0; row < 28; ++row)
            backend->messages.append(makeMessage(QString::number(10 + row), QStringLiteral("Сообщение в истории")));
        auto noPreview = makeMessage(QStringLiteral("90"), QStringLiteral("Файл остаётся доступен"));
        noPreview.files = {makeFile(QStringLiteral("3"), unavailable)};
        backend->messages.append(noPreview);
        auto own = makeMessage(QStringLiteral("91"), QStringLiteral("Мой снимок"), true);
        own.files = {makeFile(QStringLiteral("2"), visible)};
        backend->messages.append(own);
        auto quoted = makeMessage(QStringLiteral("92"), QStringLiteral("Спасибо за снимок"));
        quoted.replyId = own.id;
        quoted.replyFiles = {own.files.front(), makeFile(QStringLiteral("4"), omittedQuotePreview)};
        quoted.forwarded = true;
        quoted.forwardedAuthor = QStringLiteral("Михаил");
        backend->messages.append(quoted);

        MainWindow window(backend.get());
        window.resize(960, 790);
        window.show();
        auto *history = findAccessible<QListWidget>(&window, QStringLiteral("Сообщения чата"));
        QVERIFY(history);
        QTRY_COMPARE(backend->imageRequests.count(visible), 1);
        QTRY_COMPARE(backend->imageRequests.count(unavailable), 1);
        QCOMPARE(backend->imageRequests.count(distant), 0);
        QCOMPARE(backend->imageRequests.count(omittedQuotePreview), 0);
        const auto *last = history->item(history->count() - 1);
        const auto *previous = history->item(history->count() - 2);
        QTRY_VERIFY(history->visualItemRect(last).height() < 165);
        QTRY_VERIFY(history->visualItemRect(previous).height() > 180);
        QCOMPARE(last->data(workspace::ReplyFiles).value<QVector<MessageFile>>().front().previewUrl, visible);

        const auto capture = qEnvironmentVariable("B24_CHAT_PREVIEW_SCREENSHOT");
        if (!capture.isEmpty())
            QVERIFY(window.grab().save(capture));
        const auto photoPixel = [photoColor, history](const QRect &row) {
            const auto screenshot = history->viewport()->grab().toImage();
            const auto area = row.intersected(screenshot.rect());
            for (int y = area.top(); y <= area.bottom(); ++y)
                for (int x = area.left(); x <= area.right(); ++x)
                    if (screenshot.pixelColor(x, y) == photoColor)
                        return QPoint(x, y);
            return QPoint();
        };
        const auto quotedPhoto = photoPixel(history->visualItemRect(last));
        QVERIFY(!quotedPhoto.isNull());
        QCOMPARE(backend->photoDownloads, 0);
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, quotedPhoto);
        QTRY_COMPARE(history->currentItem(), previous);
        QVERIFY(!window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        QCOMPARE(backend->photoDownloads, 0);
        const auto ownPhoto = photoPixel(history->visualItemRect(previous));
        QVERIFY(!ownPhoto.isNull());
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, ownPhoto);
        QTRY_VERIFY(window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        auto *viewer = window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer"));
        QCOMPARE(viewer->parentWidget(), static_cast<QWidget *>(&window));
        const auto *selectedBeforeViewerKey = history->currentItem();
        QTest::keyClick(history, Qt::Key_Up);
        QCOMPARE(history->currentItem(), selectedBeforeViewerKey);
        QVERIFY(viewer->isVisible());
        auto *originalState = findAccessible<QLabel>(viewer, QStringLiteral("Состояние загрузки фотографии"));
        QVERIFY(originalState && originalState->isVisible());
        QCOMPARE(originalState->text(), QStringLiteral("Загружаем оригинал фотографии…"));
        QCOMPARE(backend->photoDownloads, 1);
        QCOMPARE(backend->photoUrl, own.files.front().downloadUrl);
        auto *zoom = findAccessible<QPushButton>(viewer, QStringLiteral("Увеличить фотографию"));
        auto *fit = findAccessible<QPushButton>(viewer, QStringLiteral("Вписать фотографию в окно"));
        auto *scale = findAccessible<QLabel>(viewer, QStringLiteral("Масштаб фотографии"));
        auto *canvas = viewer->findChild<QWidget *>(QStringLiteral("chatPhotoCanvas"));
        QVERIFY(zoom && fit && scale && canvas);
        QVERIFY(!canvas->isVisible() && !zoom->isEnabled());
        const auto firstTarget = backend->photoPath;
        const auto originalDir = QFileInfo(firstTarget).absolutePath();
        QVERIFY(QFileInfo::exists(originalDir));
        const QColor originalColor("#33688c");
        const QSize originalSize(1440, 1000);
        {
            QFile file(firstTarget);
            QVERIFY(file.open(QIODevice::WriteOnly));
            QImage original(originalSize, QImage::Format_RGB32);
            original.fill(originalColor);
            QVERIFY(original.save(&file, "PNG"));
        }
        emit backend->messageFileDownloaded(backend->photoUrl, firstTarget);
        QTRY_VERIFY(canvas->isVisible() && !originalState->isVisible());
        QCOMPARE(static_cast<ui::ChatPhotoCanvas *>(canvas)->imageSize(), originalSize);
        const auto originalScreenshot = canvas->grab().toImage();
        QCOMPARE(originalScreenshot.pixelColor(originalScreenshot.rect().center()), originalColor);
        QTRY_VERIFY(!QFileInfo::exists(originalDir));
        QCOMPARE(scale->text(), QStringLiteral("×1.00"));
        QTest::mouseClick(zoom, Qt::LeftButton);
        QCOMPARE(scale->text(), QStringLiteral("×1.25"));
        const auto center = canvas->rect().center();
        QWheelEvent wheel(center, canvas->mapToGlobal(center), {}, QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(canvas, &wheel);
        QCOMPARE(scale->text(), QStringLiteral("×1.56"));
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, center);
        QCOMPARE(canvas->cursor().shape(), Qt::ClosedHandCursor);
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, center);
        QTest::mouseClick(fit, Qt::LeftButton);
        QCOMPARE(scale->text(), QStringLiteral("×1.00"));
        const auto viewerCapture = qEnvironmentVariable("B24_CHAT_PHOTO_VIEWER_SCREENSHOT");
        if (!viewerCapture.isEmpty())
            QVERIFY(viewer->grab().save(viewerCapture));
        QTest::keyClick(viewer, Qt::Key_Escape);
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        QVERIFY(backend->downloadedUrl.isEmpty());
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, ownPhoto);
        QTRY_VERIFY(window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        viewer = window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer"));
        QCOMPARE(viewer->windowTitle(), own.files.front().name);
        QCOMPARE(backend->photoDownloads, 2);
        QCOMPARE(backend->photoUrl, own.files.front().downloadUrl);
        const auto canceledPath = backend->photoPath;
        auto *save = findAccessible<QPushButton>(viewer, QStringLiteral("Сохранить оригинал фотографии"));
        QVERIFY(save);
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_VERIFY(viewer->findChild<QFileDialog *>(QStringLiteral("messageFileSaveDialog")));
        viewer->findChild<QFileDialog *>(QStringLiteral("messageFileSaveDialog"))->reject();
        QTest::keyClick(viewer, Qt::Key_Escape);
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        QVERIFY(backend->downloadedUrl.isEmpty());
        QCOMPARE(backend->photoCancels, QStringList{canceledPath});
        QVERIFY(!QFileInfo::exists(QFileInfo(canceledPath).absolutePath()));
        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, ownPhoto);
        QTRY_VERIFY(window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        viewer = window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer"));
        originalState = findAccessible<QLabel>(viewer, QStringLiteral("Состояние загрузки фотографии"));
        QVERIFY(originalState && originalState->isVisible());
        emit backend->messagePhotoDownloadFailed(canceledPath, QStringLiteral("Устаревший ответ"));
        QCOMPARE(originalState->text(), QStringLiteral("Загружаем оригинал фотографии…"));
        emit backend->messagePhotoDownloadFailed(backend->photoPath, QStringLiteral("Сбой загрузки оригинала"));
        QCOMPARE(originalState->text(), QStringLiteral("Сбой загрузки оригинала"));
        QVERIFY(!viewer->findChild<QWidget *>(QStringLiteral("chatPhotoCanvas"))->isVisible());
        QVERIFY(findAccessible<QPushButton>(viewer, QStringLiteral("Сохранить оригинал фотографии")));
        QVERIFY(!QFileInfo::exists(QFileInfo(backend->photoPath).absolutePath()));
        QTest::keyClick(viewer, Qt::Key_Escape);
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));

        QTest::mouseClick(history->viewport(), Qt::LeftButton, Qt::NoModifier, ownPhoto);
        QTRY_VERIFY(window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        viewer = window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer"));
        auto *previousPhoto = findAccessible<QPushButton>(viewer, QStringLiteral("Предыдущая фотография"));
        auto *nextPhoto = findAccessible<QPushButton>(viewer, QStringLiteral("Следующая фотография"));
        auto *position = findAccessible<QLabel>(viewer, QStringLiteral("Номер фотографии"));
        QVERIFY(previousPhoto && nextPhoto && position);
        QCOMPARE(position->text(), QStringLiteral("3 из 3"));
        QVERIFY(previousPhoto->isEnabled() && !nextPhoto->isEnabled());
        QCOMPARE(backend->photoDownloads, 4);
        const auto quoteTarget = backend->photoPath;

        QVERIFY(QTest::qWaitForWindowExposed(viewer));
        QApplication::setActiveWindow(viewer);
        QTest::keyClick(viewer, Qt::Key_Left);
        QTRY_COMPARE(viewer->windowTitle(), noPreview.files.front().name);
        QCOMPARE(position->text(), QStringLiteral("2 из 3"));
        QCOMPARE(backend->photoUrl, noPreview.files.front().downloadUrl);
        QCOMPARE(backend->photoDownloads, 5);
        QVERIFY(backend->photoCancels.contains(quoteTarget));
        originalState = findAccessible<QLabel>(viewer, QStringLiteral("Состояние загрузки фотографии"));
        emit backend->messagePhotoDownloadFailed(quoteTarget, QStringLiteral("Устаревший ответ"));
        QCOMPARE(originalState->text(), QStringLiteral("Загружаем оригинал фотографии…"));

        QTest::mouseClick(previousPhoto, Qt::LeftButton);
        QCOMPARE(position->text(), QStringLiteral("1 из 3"));
        QCOMPARE(backend->photoUrl, top.files.front().downloadUrl);
        QCOMPARE(backend->photoDownloads, 6);
        QVERIFY(!previousPhoto->isEnabled() && nextPhoto->isEnabled());
        QTest::keyClick(viewer, Qt::Key_Left);
        QCOMPARE(backend->photoDownloads, 6);
        QTest::mouseClick(nextPhoto, Qt::LeftButton);
        QCOMPARE(backend->photoUrl, noPreview.files.front().downloadUrl);
        QCOMPARE(backend->photoDownloads, 7);
        QTest::keyClick(viewer, Qt::Key_Escape);
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));

        history->scrollToTop();
        QTRY_COMPARE(backend->imageRequests.count(distant), 1);
        emit backend->chatMessagesLoaded(QStringLiteral("c-1"), backend->messages);
        QCOMPARE(backend->imageRequests.count(visible), 1);
        QCOMPARE(backend->imageRequests.count(unavailable), 1);

        auto taskMessage = makeMessage(QStringLiteral("601"), QStringLiteral("Фото задачи"));
        taskMessage.files = {makeFile(QStringLiteral("task-1"), distant),
                             makeFile(QStringLiteral("task-2"), visible)};
        for (auto &file : taskMessage.files)
            file.isImage = true;
        backend->taskMessages = {taskMessage};
        window.openTaskCard(QStringLiteral("501"));
        auto *task = window.findChild<QDialog *>(QStringLiteral("taskCardDialog"));
        QVERIFY(task);
        auto *taskHistory = findAccessible<QListWidget>(task, QStringLiteral("Чат карточки"));
        QVERIFY(taskHistory);
        QTRY_COMPARE(taskHistory->count(), 1);
        taskHistory->setCurrentRow(0);
        QTest::keyClick(taskHistory, Qt::Key_Return);
        QTRY_VERIFY(task->findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        viewer = task->findChild<QDialog *>(QStringLiteral("chatPhotoViewer"));
        position = findAccessible<QLabel>(viewer, QStringLiteral("Номер фотографии"));
        QVERIFY(position);
        QCOMPARE(position->text(), QStringLiteral("1 из 2"));
        QCOMPARE(backend->photoUrl, taskMessage.files.front().downloadUrl);
        QVERIFY(QTest::qWaitForWindowExposed(viewer));
        QApplication::setActiveWindow(viewer);
        QTest::keyClick(viewer, Qt::Key_Right);
        QTRY_COMPARE(position->text(), QStringLiteral("2 из 2"));
        QCOMPARE(backend->photoUrl, taskMessage.files.back().downloadUrl);
    }

    void opensMediaPhotosInDisplayedOrderAndSavesTheCurrentOriginal()
    {
        std::unique_ptr<FixtureBackend> backend(makeFixture());
        backend->demoMode = false;
        const auto makeAsset = [](const QString &id, const QString &name) {
            ChatAsset asset;
            asset.id = id;
            asset.name = name;
            asset.file.id = id;
            asset.file.name = name;
            asset.file.isImage = true;
            asset.file.previewUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/preview/shared"));
            asset.file.downloadUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/download/%1").arg(id));
            return asset;
        };
        const auto first = makeAsset(QStringLiteral("7"), QStringLiteral("Первое.png"));
        const auto second = makeAsset(QStringLiteral("1"), QStringLiteral("Второе.png"));
        const auto last = makeAsset(QStringLiteral("3"), QStringLiteral("Третье.png"));
        auto preview = makeAsset(QStringLiteral("4"), QStringLiteral("Миниатюра.png"));
        preview.file.downloadUrl.clear();
        preview.file.previewUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/preview/4"));
        QByteArray bytes;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QImage thumbnail(60, 40, QImage::Format_RGB32);
        thumbnail.fill(Qt::blue);
        QVERIFY(thumbnail.save(&buffer, "PNG"));
        backend->messageImages.insert(preview.file.previewUrl, bytes);
        backend->mediaAssets = {first, second, last, preview};
        MainWindow window(backend.get());
        window.show();
        QApplication::setActiveWindow(&window);
        emit backend->connectionChanged(true);
        auto *assets = findAccessible<QPushButton>(&window, QStringLiteral("Медиа, файлы и ссылки чата"));
        QVERIFY(assets);
        assets->click();
        auto *media = findAccessible<QListWidget>(&window, QStringLiteral("Медиа"));
        QVERIFY(media);
        QTRY_COMPARE(media->count(), 4);
        media->setCurrentRow(1);
        QTest::keyClick(media, Qt::Key_Return);
        QTRY_VERIFY(window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        auto *viewer = window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer"));
        auto *position = findAccessible<QLabel>(viewer, QStringLiteral("Номер фотографии"));
        auto *next = findAccessible<QPushButton>(viewer, QStringLiteral("Следующая фотография"));
        auto *save = findAccessible<QPushButton>(viewer, QStringLiteral("Сохранить оригинал фотографии"));
        QVERIFY(position && next && save);
        QCOMPARE(position->text(), QStringLiteral("2 из 4"));
        QCOMPARE(backend->photoUrl, second.file.downloadUrl);
        next->click();
        QCOMPARE(position->text(), QStringLiteral("3 из 4"));
        QCOMPARE(viewer->windowTitle(), last.file.name);
        QCOMPARE(backend->photoUrl, last.file.downloadUrl);
        QVERIFY(next->isEnabled());
        save->click();
        auto *fileDialog = viewer->findChild<QFileDialog *>(QStringLiteral("messageFileSaveDialog"));
        QVERIFY(fileDialog);
        QCOMPARE(QFileInfo(fileDialog->selectedFiles().value(0)).fileName(), last.file.name);
        fileDialog->reject();
        QVERIFY(!backend->imageRequests.contains(preview.file.previewUrl));
        const auto downloads = backend->photoDownloads;
        next->click();
        QCOMPARE(position->text(), QStringLiteral("4 из 4"));
        QCOMPARE(backend->photoDownloads, downloads);
        QVERIFY(!next->isEnabled() && !save->isVisible());
        auto *canvas = viewer->findChild<QWidget *>(QStringLiteral("chatPhotoCanvas"));
        QVERIFY(canvas);
        QTRY_VERIFY(canvas->isVisible());
        QCOMPARE(backend->imageRequests.count(preview.file.previewUrl), 1);
        auto *previous = findAccessible<QPushButton>(viewer, QStringLiteral("Предыдущая фотография"));
        QVERIFY(previous);
        const auto center = canvas->rect().center();
        QContextMenuEvent photoMenuEvent(QContextMenuEvent::Mouse, center, canvas->mapToGlobal(center));
        QCoreApplication::sendEvent(canvas, &photoMenuEvent);
        QTRY_VERIFY(visibleMessageMenu(QStringLiteral("imageContextMenu")));
        const QPointer<QAction> oldCopy(visibleMessageMenu(QStringLiteral("imageContextMenu"))->actions().last());
        auto *clipboard = QGuiApplication::clipboard();
        clipboard->setText(QStringLiteral("previous frame sentinel"));
        previous->click();
        QVERIFY(!visibleMessageMenu(QStringLiteral("imageContextMenu")));
        if (oldCopy)
            oldCopy->trigger();
        QCOMPARE(clipboard->text(), QStringLiteral("previous frame sentinel"));
        QCOMPARE(backend->photoUrl, last.file.downloadUrl);
        const auto canceled = backend->photoPath;
        backend->accountScope = QStringLiteral("fixture/photo-account-changed");
        emit backend->accountScopeChanged();
        QTRY_VERIFY(!window.findChild<QDialog *>(QStringLiteral("chatPhotoViewer")));
        QVERIFY(backend->photoCancels.contains(canceled));
    }

    void historyEnterOpensPhotoLikeMouse()
    {
        ui::HistoryList list(nullptr);
        workspace::configureRows(&list, workspace::Row::Message);
        list.resize(420, 320);
        list.show();
        auto *item = workspace::addMarkupRow(&list, {}, QStringLiteral("Фотография"), {}, QStringLiteral("m-1"));
        MessageFile file;
        file.id = QStringLiteral("f-1");
        file.name = QStringLiteral("photo.jpg");
        file.isImage = true;
        file.downloadUrl = QUrl(QStringLiteral("https://example.bitrix24.ru/photo.jpg"));
        item->setData(workspace::MessageFiles, QVariant::fromValue(QVector<MessageFile>{file}));
        list.setCurrentRow(0);
        list.setFocus();
        QTRY_VERIFY(list.hasFocus());

        int opened = 0;
        QString openedId;
        auto *delegate = static_cast<workspace::RowPainter *>(list.itemDelegate());
        QVERIFY(delegate);
        delegate->setMessagePhotoHandler([&opened, &openedId](const MessageFile &entry) {
            ++opened;
            openedId = entry.id;
        });
        QTest::keyClick(&list, Qt::Key_Return);
        QCOMPARE(opened, 1);
        QCOMPARE(openedId, QStringLiteral("f-1"));
    }
};

QTEST_MAIN(UiChatMediaTest)
#include "tst_ui_chat_media.moc"
