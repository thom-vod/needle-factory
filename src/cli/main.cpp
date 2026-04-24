#include <cstdlib>
#include <atomic>
#include <iostream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#endif

#ifdef NEEDLE_HAS_CURL
#include <curl/curl.h>
#endif

#include "router.h"

namespace {
    std::atomic<bool> g_cancelled(false);
#ifndef _WIN32
    volatile sig_atomic_t g_signal_received = 0;
#else
    volatile long g_signal_received = 0;
#endif
}

#ifndef _WIN32

static void signal_handler(int signum) {
    g_signal_received = signum;
    g_cancelled.store(true);
}

#else

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            g_signal_received = 2; // SIGINT equivalent
            g_cancelled.store(true);
            return TRUE;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_signal_received = 15; // SIGTERM equivalent
            g_cancelled.store(true);
            return TRUE;
        default:
            return FALSE;
    }
}

#endif

int main(int argc, char* argv[]) {
#ifdef NEEDLE_HAS_CURL
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif

#ifndef _WIN32
    // Ignore SIGPIPE
    struct sigaction sa_pipe;
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sa_pipe.sa_flags = 0;
    sigaction(SIGPIPE, &sa_pipe, nullptr);

    // Install SIGINT handler
    struct sigaction sa_int;
    sa_int.sa_handler = signal_handler;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = 0;
    sigaction(SIGINT, &sa_int, nullptr);

    // Install SIGTERM handler
    struct sigaction sa_term;
    sa_term.sa_handler = signal_handler;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = 0;
    sigaction(SIGTERM, &sa_term, nullptr);
#else
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#endif

    needle::Router router(g_cancelled);
    int exit_code = router.dispatch(argc, argv);

    // If we received a signal, use the conventional exit code
    if (g_signal_received == 2) {  // SIGINT
        exit_code = 130;
    } else if (g_signal_received == 15) {  // SIGTERM
        exit_code = 143;
    }

#ifdef NEEDLE_HAS_CURL
    curl_global_cleanup();
#endif

    return exit_code;
}
