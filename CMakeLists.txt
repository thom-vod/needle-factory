cmake_minimum_required(VERSION 3.10)
project(needle VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(NEEDLE_BUILD_SERVER "Build the HTTP server" ON)
option(NEEDLE_BUILD_TESTS "Build tests" ON)
option(NEEDLE_ASAN "Enable AddressSanitizer" OFF)
option(NEEDLE_TSAN "Enable ThreadSanitizer" OFF)

# Strict warnings — use appropriate flags for each compiler
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# Sanitizers (GCC/Clang only)
if(NOT MSVC)
    if(NEEDLE_ASAN)
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()

    if(NEEDLE_TSAN)
        add_compile_options(-fsanitize=thread)
        add_link_options(-fsanitize=thread)
    endif()
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(SYSTEM ${CMAKE_SOURCE_DIR}/third_party)

# ---------- libneedle (static library) ----------

set(NEEDLE_LIB_SOURCES
    src/config/needle_config.cpp
    src/model/attribute_map.cpp
    src/model/graph.cpp
    src/model/outcome.cpp
    src/model/context.cpp
    src/model/retry_policy.cpp
    src/model/fidelity.cpp
    src/parser/dot_lexer.cpp
    src/parser/dot_parser.cpp
    src/parser/graph_builder.cpp
    src/parser/condition_parser.cpp
    src/parser/stylesheet_parser.cpp
    src/validation/diagnostic.cpp
    src/validation/graph_validator.cpp
    src/validation/dot_linter.cpp
    src/validation/rules/single_start_node.cpp
    src/validation/rules/single_exit_node.cpp
    src/validation/rules/start_no_incoming.cpp
    src/validation/rules/exit_no_outgoing.cpp
    src/validation/rules/all_nodes_reachable.cpp
    src/validation/rules/valid_conditions.cpp
    src/validation/rules/parallel_fan_in_pairing.cpp
    src/validation/rules/no_self_loops.cpp
    src/validation/rules/params_attribute_syntax.cpp
    src/event/event.cpp
    src/event/event_bus.cpp
    src/event/collector_event_bus.cpp
    src/engine/edge_selector.cpp
    src/engine/checkpoint_manager.cpp
    src/engine/stage_advancer.cpp
    src/engine/retry_controller.cpp
    src/engine/stylesheet_transform.cpp
    src/engine/variable_expansion_transform.cpp
    src/engine/resume_validator.cpp
    src/engine/auto_troubleshoot.cpp
    src/engine/run_guard.cpp
    src/engine/troubleshoot_agent.cpp
    src/engine/troubleshoot_backup.cpp
    src/engine/recovery_report.cpp
    src/engine/fan_in_merger.cpp
    src/engine/pipeline_engine.cpp
    src/engine/subgraph_topology.cpp
    src/handlers/handler_base.cpp
    src/handlers/handler_registry.cpp
    src/handlers/start_handler.cpp
    src/handlers/exit_handler.cpp
    src/handlers/codergen_handler.cpp
    src/handlers/llmkit_handler.cpp
    src/handlers/conditional_handler.cpp
    src/handlers/parallel_handler.cpp
    src/handlers/fan_in_handler.cpp
    src/handlers/wait_human_handler.cpp
    src/handlers/tool_handler.cpp
    src/handlers/manager_loop_handler.cpp
    src/handlers/web_search_handler.cpp
    src/handlers/doc_fetch_handler.cpp
    src/handlers/nested_run_handler.cpp
    src/handlers/interactive_handler.cpp
    src/backend/process_runner.cpp
    src/backend/prompt_assembly_policy.cpp
    src/backend/cli_backend.cpp
    src/backend/llmkit_backend.cpp
    src/interviewer/console_interviewer.cpp
    src/interviewer/auto_approve_interviewer.cpp
    src/interviewer/queue_interviewer.cpp
    src/interviewer/recording_interviewer.cpp
    src/util/graph_serializer.cpp
    src/util/context_defaults.cpp
    src/util/fs_helpers.cpp
    src/util/run_registry.cpp
    src/util/resource_locator.cpp
    src/util/logger.cpp
    src/util/curl_client.cpp
    src/util/git_state.cpp
    src/util/git_helpers.cpp
    src/util/timestamp.cpp
    src/troubleshoot/diagnose.cpp
    src/troubleshoot/types.cpp
    src/troubleshoot/allowed_tools.cpp
    src/troubleshoot/session_id.cpp
    src/troubleshoot/stream_parser.cpp
    src/troubleshoot/write_hook.cpp
    src/worktree/strategy.cpp
    src/worktree/manager.cpp
    src/server/dot_authoring_rules_embedded.cpp
    src/server/templates_embedded.cpp
)

# Platform-specific sources
if(WIN32)
    list(APPEND NEEDLE_LIB_SOURCES
        src/platform/platform_win32.cpp
        src/backend/process_runner_win32.cpp
    )
else()
    list(APPEND NEEDLE_LIB_SOURCES
        src/platform/platform_posix.cpp
    )
endif()

# Server sources (conditional)
if(NEEDLE_BUILD_SERVER)
    list(APPEND NEEDLE_LIB_SOURCES
        src/server/http_server.cpp
        src/server/model_cache.cpp
        src/server/dashboard_html.cpp
        src/server/dot_generator.cpp
        src/interviewer/http_interviewer.cpp
    )

    # Regenerate dashboard_html.cpp if Python3 available and assets changed
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_custom_command(
            OUTPUT ${CMAKE_SOURCE_DIR}/src/server/dashboard_html.cpp
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/embed_html.py
            DEPENDS ${CMAKE_SOURCE_DIR}/src/server/assets/dashboard.html
                    ${CMAKE_SOURCE_DIR}/src/server/assets/dashboard.css
                    ${CMAKE_SOURCE_DIR}/src/server/assets/dashboard.js
                    ${CMAKE_SOURCE_DIR}/scripts/embed_html.py
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Regenerating dashboard HTML constants"
        )
        add_custom_command(
            OUTPUT ${CMAKE_SOURCE_DIR}/src/server/dot_authoring_rules_embedded.cpp
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/embed_rules.py
            DEPENDS ${CMAKE_SOURCE_DIR}/docs/dot-authoring-rules.md
                    ${CMAKE_SOURCE_DIR}/scripts/embed_rules.py
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Regenerating DOT authoring rules constants"
        )
        file(GLOB NEEDLE_TEMPLATE_SOURCES ${CMAKE_SOURCE_DIR}/sample_dots/*.dot)
        add_custom_command(
            OUTPUT ${CMAKE_SOURCE_DIR}/src/server/templates_embedded.cpp
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/embed_templates.py
            DEPENDS ${CMAKE_SOURCE_DIR}/scripts/embed_templates.py
                    ${NEEDLE_TEMPLATE_SOURCES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Regenerating bundled template constants"
        )
    endif()
endif()

add_library(needle_lib STATIC ${NEEDLE_LIB_SOURCES})
target_include_directories(needle_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_include_directories(needle_lib SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party
)
# Also add src/ for internal headers like router.h, http_interviewer.h
target_include_directories(needle_lib PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

if(NEEDLE_BUILD_SERVER)
    target_compile_definitions(needle_lib PUBLIC NEEDLE_ENABLE_SERVER=1)
endif()

find_package(Threads REQUIRED)
target_link_libraries(needle_lib PUBLIC Threads::Threads)

find_package(CURL QUIET)
if(CURL_FOUND)
    target_link_libraries(needle_lib PUBLIC CURL::libcurl)
    target_compile_definitions(needle_lib PUBLIC NEEDLE_HAS_CURL=1)
endif()

# Windows-specific libraries
if(WIN32)
    target_link_libraries(needle_lib PUBLIC ws2_32)
endif()

# ---------- needle (executable) ----------

add_executable(needle
    src/cli/main.cpp
    src/cli/router.cpp
)
target_include_directories(needle PRIVATE
    ${CMAKE_SOURCE_DIR}/src/cli
)
target_link_libraries(needle PRIVATE needle_lib)

# ---------- needle_tests ----------

if(NEEDLE_BUILD_TESTS)
    set(NEEDLE_TEST_SOURCES
        tests/test_main.cpp
        tests/test_result.cpp
        tests/test_maybe.cpp
        tests/test_attribute_map.cpp
        tests/test_context.cpp
        tests/test_retry_policy.cpp
        tests/test_dot_lexer.cpp
        tests/test_dot_parser.cpp
        tests/test_graph_builder.cpp
        tests/test_condition_parser.cpp
        tests/test_stylesheet_parser.cpp
        tests/test_validator.cpp
        tests/test_dot_linter.cpp
        tests/test_event_bus.cpp
        tests/test_edge_selector.cpp
        tests/test_checkpoint.cpp
        tests/test_retry_controller.cpp
        tests/test_pipeline_engine.cpp
        tests/test_stylesheet_transform.cpp
        tests/test_variable_expansion.cpp
        tests/test_skill_corpus.cpp
        tests/test_process_runner.cpp
        tests/test_cli_backend.cpp
        tests/test_prompt_assembly.cpp
        tests/test_llmkit_backend.cpp
        tests/test_codergen_handler.cpp
        tests/test_parallel_handler.cpp
        tests/test_fan_in_handler.cpp
        tests/test_conditional_handler.cpp
        tests/test_wait_human_handler.cpp
        tests/test_tool_handler.cpp
        tests/test_manager_loop_handler.cpp
        tests/test_web_search_handler.cpp
        tests/test_doc_fetch_handler.cpp
        tests/test_nested_run_handler.cpp
        tests/test_graph_serializer.cpp
        tests/test_resume_validator.cpp
        tests/test_resource_locator.cpp
        tests/test_logger.cpp
        tests/test_run_registry.cpp
        tests/test_dry_run_isolation.cpp
        tests/test_needle_config.cpp
        tests/test_resume_config_defaults.cpp
        tests/test_git_state.cpp
        tests/test_stage_advancer.cpp
        tests/test_troubleshoot_diagnose.cpp
        tests/test_troubleshoot_types.cpp
        tests/test_troubleshoot_compat.cpp
        tests/test_troubleshoot_session_id.cpp
        tests/test_allowed_tools.cpp
        tests/test_allowed_tools_smoke.cpp
        tests/test_stream_parser.cpp
        tests/test_write_hook.cpp
        tests/test_auto_troubleshoot.cpp
        tests/test_run_guard.cpp
        tests/test_troubleshoot_escalation.cpp
        tests/test_recovery_report_v2.cpp
        tests/test_troubleshoot_agent.cpp
        tests/test_troubleshoot_backup.cpp
        tests/test_worktree_strategy.cpp
        tests/test_worktree_manager.cpp
        tests/test_fidelity.cpp
        tests/test_subgraph_topology.cpp
        tests/test_model_cache.cpp
        tests/test_cli.cpp
        tests/test_http_server.cpp
        tests/test_troubleshoot_endpoint.cpp
        tests/test_dashboard_endpoints.cpp
        tests/test_server_integration.cpp
        tests/integration/test_full_pipeline.cpp
        tests/integration/test_checkpoint_resume.cpp
        tests/integration/test_parallel.cpp
        tests/integration/test_parallel_merge.cpp
        tests/integration/test_regression_stress.cpp
        tests/integration/test_auto_troubleshoot_integration.cpp
        tests/integration/test_worktree_integration.cpp
    )

    add_executable(needle_tests
        ${NEEDLE_TEST_SOURCES}
        src/cli/router.cpp
    )
    target_include_directories(needle_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/src/cli
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(needle_tests PRIVATE needle_lib)

    # Pass source dir to tests for finding sample DOT files
    target_compile_definitions(needle_tests PRIVATE
        NEEDLE_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
endif()

# ---------- Install rules ----------

include(GNUInstallDirs)
install(TARGETS needle RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY sample_dots/ DESTINATION ${CMAKE_INSTALL_DATADIR}/needle/sample_dots
    FILES_MATCHING PATTERN "*.dot" PATTERN "*.nss")
install(DIRECTORY scripts/ DESTINATION ${CMAKE_INSTALL_DATADIR}/needle/scripts
    FILES_MATCHING PATTERN "*.sh" PATTERN "*.py"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
