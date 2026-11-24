# Запросы облачного Битрикс24

Клиент повторяет запросы веб-интерфейса с действующей браузерной сессией. Порталу передаются его Cookie и CSRF-токен. Для подключения не требуется регистрировать приложение, выдавать OAuth-доступ или создавать вебхук.

| Сценарий | Действие веб-интерфейса |
|---|---|
| Недавние чаты | `im.v2.Recent.load` |
| Открытие диалога и чтение сообщений | `im.v2.Chat.load`, `im.v2.Chat.Message.list` |
| Отправка текста | `im.v2.Chat.Message.send` с `dialogId` и полями `message`, `templateId` |
| Проекты | `socialnetwork.api.workgroup.list` |
| Колонки и задачи канбана | `bitrix:tasks.kanban` → `applyFilter` с параметрами выбранной группы |
| Карточка задачи | `tasks.task.get` |
| Чат задачи | `CHAT_ID` из карточки определяет диалог `chat<CHAT_ID>` |

Действия идут через `/bitrix/services/main/ajax.php`. Для компонента канбана запрос включает `mode=class` и `c=bitrix:tasks.kanban`. Канбан отображает колонки и их порядок из `data.columns`, включая пустые. Элементы из `data.items` распределяются по `columnId`.

Веб-сайт получает начальную конфигурацию Push&Pull запросом `/rest/pull.config.get.json`, а историю уведомлений вызывает через `/rest/batch.json` с `im.notify.get`. В обоих случаях это запрос того же портала с веб-сессией. После загрузки конфигурации клиент открывает защищённый WebSocket с параметрами подписки, не передавая портальные Cookie на сервер событий.

HTTP-запросы клиента привязаны к HTTPS-адресу портала. Приложение отклоняет перенаправления и не включает секреты в сообщения об ошибках. Режим локальных тестов отдельно разрешает loopback.

Источники по отправке сообщений, канбану и Push&Pull. [Код отправки](https://dev.1c-bitrix.ru/bitrix/js/im/v2/provider/service/sending/dist/sending.bundle.js) · [Канбан в проектах](https://helpdesk.bitrix24.ru/open/19507218/) · [Код Push&Pull](https://helpdesk.bitrix24.ru/bitrix/js/pull/client/pull.client.js) · [Описание Push&Pull](https://apidocs.bitrix24.com/settings/interactivity/custom-push-and-pull-client.html)
