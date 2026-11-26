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
    src/cloud/cloud_rows.cpp
    src/cloud/tasks_service.cpp
    src/cloud/projects_service.cpp
    src/cloud/projects_service_board.cpp
    src/cloud/projects_service_chats.cpp
    src/cloud/cloud_backend.cpp
    src/cloud/cloud_backend_facade.cpp
    src/cloud/cloud_link_previews.cpp
    src/cloud/cloud_channels.cpp
    src/cloud/cloud_chat_fields.cpp
    src/cloud/cloud_chat_metadata.cpp
    src/cloud/chat_timeline.cpp
    src/cloud/cloud_chat_assets.cpp
    src/cloud/cloud_message_uploads.cpp
    src/cloud/cloud_message_downloads.cpp
    src/cloud/cloud_message_decoder.cpp
    src/cloud/cloud_avatars.cpp
    src/cloud/cloud_notification_links.cpp
    src/cloud/notifications_service.h
    src/cloud/notifications_service.cpp
    src/cloud/notification_popups.cpp
    src/cloud/pull_channel.h
    src/cloud/pull_channel.cpp
    src/cloud/cloud_people.cpp
    src/cloud/cloud_chat_badges.cpp
    src/cloud/chats_service.cpp
    src/cloud/chats_service_pull.cpp
    src/cloud/chats_service_history.cpp
    src/cloud/chats_service_search.cpp
    src/cloud/chats_service_actions.cpp
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
    src/ui/notifications_panel.h
    src/ui/notifications_panel.cpp
    src/ui/projects_panel.cpp
    src/ui/projects_panel.h
    src/ui/workspace_presentation.cpp
    src/ui/task_card_controller.h
    src/ui/task_card_controller.cpp
    src/ui/task_card_controller_events.cpp
    src/ui/task_card_controller_errors.cpp
    src/ui/task_card_controller_history.cpp
    src/ui/task_card_controller_files.cpp
    src/ui/main_window.cpp
    src/ui/avatar_painter.cpp
    src/ui/card_row_painter.cpp
    src/ui/directory_row_painter.cpp
    src/ui/file_preview_painter.cpp
    src/ui/file_row_layout.cpp
    src/ui/link_preview_painter.cpp
    src/ui/message_row_layout.cpp
    src/ui/message_row_painter.cpp
    src/ui/notice_row_painter.cpp
    src/ui/preview_animation.cpp
    src/ui/row_interaction.cpp
    src/ui/row_paint_primitives.cpp
    src/ui/row_painter.cpp
    src/ui/row_text_selection.cpp
    src/ui/workspace_directory.cpp
    src/ui/workspace_row_style.cpp
    src/ui/workspace_rows.cpp
    src/ui/workspace_theme.cpp
    src/ui/avatar_badge.cpp
    src/ui/chat_composer.cpp
    src/ui/chat_image_previews.cpp
    src/ui/chat_photo_canvas.cpp
    src/ui/chat_photo_dialog.cpp
    src/ui/chat_text_edit.cpp
    src/ui/conversation_canvas.cpp
    src/ui/emoji_panel.cpp
    src/ui/emoji_tile.cpp
    src/ui/gantt_rows.cpp
    src/ui/glyph_button.cpp
    src/ui/history_list.cpp
    src/ui/history_list_interaction.cpp
    src/ui/loading_clock.cpp
    src/ui/loading_indicator.cpp
    src/ui/mention_suggestion_delegate.cpp
    src/ui/name_dialog.cpp
    src/ui/people_suggestions.cpp
    src/ui/photo_viewer_input.cpp
    src/ui/pinned_message_bar.cpp
    src/ui/portal_dnd_control.cpp
    src/ui/reaction_palette.cpp
    src/ui/requisite_avatar.cpp
    src/ui/section_rail_painter.cpp
    src/ui/stage_manager_dialog.cpp
    src/ui/task_card_actions.cpp
    src/ui/task_card_dialog.cpp
    src/ui/task_card_requisites.cpp
    src/ui/task_content_list.cpp
    src/ui/task_description/bb_code.cpp
    src/ui/task_description/decoder.cpp
    src/ui/task_description/input.cpp
    src/ui/task_description/mark.cpp
    src/ui/task_description/safe_document.cpp
    src/ui/task_description_editor.cpp
    src/ui/task_detail_editor.cpp
    src/ui/task_dialog_controls.cpp
    src/ui/task_edit_form.cpp
    src/ui/task_fields.cpp
    src/ui/task_gantt_view.cpp
    src/ui/task_people_input.cpp
    src/ui/task_picker_choice_delegate.cpp
    src/ui/task_picker_choices.cpp
    src/ui/task_picker_summary.cpp
    src/ui/task_picker_value_label.cpp
    src/ui/task_relations_panel.cpp
    src/ui/task_requisite_row.cpp
    src/ui/task_selection_field.cpp
    src/ui/composer_send_controller.cpp
    src/ui/chat_panel.h
    src/ui/chat_panel.cpp
    src/ui/chat_panel_directory.cpp
    src/ui/chat_panel_history.cpp
    src/ui/chat_panel_search.cpp
    src/ui/chat_panel_composer.cpp
    src/ui/chat_panel_events.cpp
    src/ui/chat_panel_actions.cpp
    src/ui/history_surface.h
    src/ui/history_surface.cpp
    src/ui/message_files.cpp
    src/ui/workspace_message_viewers.cpp
    src/ui/workspace_message_previews.cpp
    src/ui/chat_video_previews.cpp
    src/ui/message_menu_window.cpp
    src/ui/public_link_preview.cpp
    src/ui/board_widget.h
    src/ui/board_widget.cpp
    src/ui/board_card_painter.cpp
    src/ui/board_column.cpp
    src/ui/board_column_grip.cpp
    src/ui/board_columns.cpp
    src/ui/board_drag_list.cpp
    src/ui/board_paging.cpp
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
    add_executable(tst_history_surface tests/tst_history_surface.cpp)
    target_link_libraries(tst_history_surface PRIVATE bxwidgets Qt6::Test)
    add_test(NAME tst_history_surface COMMAND tst_history_surface)
    set_tests_properties(tst_history_surface PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(tst_ui_workspace_presentation tests/tst_ui_workspace_presentation.cpp)
    target_link_libraries(tst_ui_workspace_presentation PRIVATE bxwidgets Qt6::Test)
    add_test(NAME tst_ui_workspace_presentation COMMAND tst_ui_workspace_presentation)
    set_tests_properties(tst_ui_workspace_presentation PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(tst_ui_widget_access tests/tst_ui_widget_access.cpp)
    target_link_libraries(tst_ui_widget_access PRIVATE bxwidgets Qt6::Test)
    add_test(NAME tst_ui_widget_access COMMAND tst_ui_widget_access)
    set_tests_properties(tst_ui_widget_access PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    target_sources(tst_public_link_preview PRIVATE src/ui/chat_image_previews.cpp)
    target_link_libraries(tst_message_markup PRIVATE bxwidgets)
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
