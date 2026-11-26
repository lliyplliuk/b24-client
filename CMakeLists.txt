cmake_minimum_required(VERSION 3.16)
project(bitrix24_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network WebSockets)
find_package(Qt6 QUIET COMPONENTS Test)

# --- core library: models, backends, transport, push/pull -------------------
add_library(bxcore STATIC
    src/core/operations.h
    src/core/operations.cpp
    src/core/models.h
    src/core/message_markup.cpp
    src/core/worktime.h
    src/core/backend.h
    src/core/session.h
    src/signin/session_capture.h
    src/signin/session_capture.cpp
    src/signin/session_restore.h
    src/signin/session_restore.cpp
    src/core/tunables.h
    src/core/origin_guard.h
    src/core/origin_guard.cpp
    src/core/demo_backend.h
    src/core/demo_backend.cpp
    src/cloud/cloud_backend.h
    src/cloud/portal_http.h
    src/cloud/portal_http.cpp
    src/cloud/cloud_backend.cpp
    src/cloud/cloud_channels.cpp
    src/cloud/cloud_avatars.cpp
    src/cloud/cloud_notification_links.cpp
    src/cloud/cloud_people.cpp
    src/cloud/cloud_chat_badges.cpp
    src/cloud/cloud_chat_actions.cpp
    src/cloud/cloud_portal_status.cpp
    src/cloud/cloud_worktime.cpp
)
target_include_directories(bxcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bxcore PUBLIC Qt6::Core Qt6::Network Qt6::WebSockets)
if(WIN32)
    target_compile_definitions(bxcore PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# --- application ----------------------------------------------------------
add_library(bxwidgets STATIC
    src/ui/main_window.h
    src/ui/main_window.cpp
    src/ui/composer_send_controller.cpp
    src/ui/chat_action_ui.cpp
    src/ui/chat_video_previews.cpp
    src/ui/message_menu_window.cpp
    src/ui/public_link_preview.cpp
    src/ui/board_widget.h
    src/ui/board_widget.cpp
    src/ui/connection_dialog.h
    src/ui/connection_dialog.cpp
    src/ui/dialog_chrome.cpp
    src/ui/worktime_panel.cpp
)
target_link_libraries(bxwidgets PUBLIC bxcore Qt6::Widgets)

add_library(bxchromesignin STATIC
    src/chromesignin/chrome_signin.h
    src/chromesignin/chrome_signin_internal.h
    src/chromesignin/chrome_signin.cpp
    src/signin/session_store.h
    src/signin/session_store.cpp
)
target_link_libraries(bxchromesignin PUBLIC bxcore)

add_executable(bitrix24-client src/app/main.cpp src/app/desktop_notification.cpp assets/application.qrc)
set_target_properties(bitrix24-client PROPERTIES AUTORCC ON)
target_link_libraries(bitrix24-client PRIVATE bxwidgets bxchromesignin)
if(WIN32)
    target_sources(bitrix24-client PRIVATE src/app/windows_update.cpp)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    target_link_libraries(bitrix24-client PRIVATE Qt6::DBus)
endif()

if(MSVC)
    target_compile_options(bxcore PUBLIC /utf-8)
endif()

foreach(target bxcore bxchromesignin)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

# --- tests ----------------------------------------------------------------
if(Qt6Test_FOUND)
    enable_testing()
    add_subdirectory(tests)
    add_executable(tst_signin_session src/signin/session_capture_test.cpp)
    target_link_libraries(tst_signin_session PRIVATE bxchromesignin Qt6::Test)
    add_test(NAME tst_signin_session COMMAND tst_signin_session)

    add_executable(tst_session_restore tests/tst_session_restore.cpp)
    target_link_libraries(tst_session_restore PRIVATE bxchromesignin Qt6::Test)
    add_test(NAME tst_session_restore COMMAND tst_session_restore)

    add_executable(tst_chrome_signin tests/tst_chrome_signin.cpp)
    target_link_libraries(tst_chrome_signin PRIVATE bxchromesignin Qt6::Test)
    add_test(NAME tst_chrome_signin COMMAND tst_chrome_signin)

    add_executable(tst_connection_dialog tests/tst_connection_dialog.cpp)
    target_link_libraries(tst_connection_dialog PRIVATE bxwidgets Qt6::Test)
    add_test(NAME tst_connection_dialog COMMAND tst_connection_dialog)
    set_tests_properties(tst_connection_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(tst_windows_update tests/tst_windows_update.cpp src/app/windows_update.cpp)
    target_include_directories(tst_windows_update PRIVATE src)
    target_link_libraries(tst_windows_update PRIVATE Qt6::Network Qt6::Test)
    add_test(NAME tst_windows_update COMMAND tst_windows_update)
else()
    message(WARNING "Qt6 Test not found - tests are skipped")
endif()

install(TARGETS bitrix24-client RUNTIME DESTINATION bin)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(GNUInstallDirs)
    install(FILES assets/bitrix24-client.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    foreach(size 16 22 24 32 48 64 128 256)
        install(FILES "assets/icons/${size}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps"
            RENAME bitrix24-client.png)
    endforeach()
endif()
