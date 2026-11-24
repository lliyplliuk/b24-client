<p align="center">
  <img src="assets/icons/128.png" alt="Bitrix24 client logo" width="88" height="88">
</p>

# Desktop Client for Bitrix24 Cloud

An unofficial desktop client built with C++ and Qt Widgets. Keep up with your chats, project tasks, and portal notifications in one window.

## Features

- **Messaging.** Regular chats and task chats with separate unread counts, message search, pins, replies, reactions, and attachments.
- **Projects and tasks.** Work with a Kanban board, task list, or Gantt chart. Open a task's chat from its details. When creating a task, you can choose the assignee, creator, project, stage, and observers, and attach files.
- **Notifications and time tracking.** See portal notifications and track your workday in a dedicated view.

## Platforms

| Platform | Availability |
| --- | --- |
| Linux | Offline demo and access to your cloud portal. The client opens Google Chrome in a dedicated browser profile for sign-in. |
| Windows | Builds and tests run in GitHub Actions. The demo and diagnostics with an imported session are available. The built-in Chrome sign-in flow is Linux-only. |

## Quick start on Linux

On Debian and Ubuntu, install a C++17 compiler, CMake, and the Qt 6 development packages. Running the tests also requires Qt Test.

```bash
sudo apt install build-essential cmake qt6-base-dev qt6-websockets-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
./build/bin/bitrix24-client --demo
```

The demo uses sample data and runs offline without an account.

To connect to your portal, replace the example URL with your own.

```bash
./build/bin/bitrix24-client --portal https://portal.example/online/
```

At startup, the client checks the saved session for the last portal you used. If the session is missing or has expired, click **Sign in**. The Bitrix24 page opens in Chrome, where you can sign in with Yandex or enter your username and password. The client confirms your session once the browser returns to the selected portal.

On Linux, the client stores each portal's confirmed session separately in the system's Secret Service keyring using libsecret. You can still sign in through Chrome if the keyring is unavailable. If a temporary error interrupts session verification, the client keeps the saved session for the next attempt.

## Build on Windows

Install a C++17 compiler, CMake, and Qt 6 with WebSockets. If CMake cannot find Qt, set `CMAKE_PREFIX_PATH` to your Qt installation. These commands assume a Visual Studio generator.

```powershell
cmake -S . -B build
cmake --build build --config Release
.\build\bin\Release\bitrix24-client.exe --demo
```

The [GitHub Actions workflow](.github/workflows/windows-build.yml) lists the Qt and build settings used in CI.

## Run the tests

```bash
ctest --test-dir build --output-on-failure
```

If you built with a Visual Studio generator, add `-C Release` to the CTest command.

<details>
<summary>Keyboard shortcuts and navigation</summary>

- `Alt+1` opens chats, `Alt+2` opens projects, and `Alt+3` opens notifications. `F5` refreshes the current view.
- In a conversation, `Ctrl+F` opens search, `Enter` sends the message, and `Shift+Enter` inserts a line break. Clicking a chat or pressing `Enter` in the chat list focuses the message composer. Use the arrow keys to navigate the message history. When sending is available, typing in the history adds text to the draft.
- Select text with the mouse or with `Shift` and the arrow keys. `Ctrl+C` copies the selection. `Ctrl+A` selects the entire message when the cursor is inside its text. `Esc` clears the selection.
- Scroll up or click **Earlier messages** to load more history. **Back to latest** takes you to the end of the conversation. Click the pinned-message banner to jump to that message. If there are several pins, use the adjacent button to open the list.
- Click a reaction below a message to add or remove it. Open the **⋯** menu for message actions and more reactions. Your most-used reactions appear first.
- Choose **Edit…** on one of your messages to open it in the composer. `Enter` saves the edit. `Esc` cancels it and restores the previous draft and any quoted message.
- In a project, **More tasks** loads the next page for the selected stage. If a system tray is available, closing the window leaves Push&Pull connected. Choose **Open Bitrix24** from the tray to reopen the window.

</details>

<details>
<summary>Connection diagnostics</summary>

The `--session-stdin` option reads a single JSON line containing `origin`, `cookie`, and `csrf` from a private input channel and starts the client in read-only mode. Add `--probe` to check read access and Push&Pull without opening a window. Exit code `2` means that some checks did not complete.

</details>

See the [portal integration notes](docs/contracts.md) for the methods and request formats used by the client.

## Unofficial Project

This independent project has no affiliation with or support from the developer of Bitrix24. The software is provided as is, without warranty. The authors disclaim liability for its use to the extent permitted by applicable law.

## License

The project code is available under the [MIT License](LICENSE). Qt is subject to its [own licensing terms](https://www.qt.io/development/qt-framework/qt-licensing).
