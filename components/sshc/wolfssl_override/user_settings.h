/*
 * Override shim placed FIRST on the wolfSSL/wolfSSH include path. It pulls
 * the real (managed-component) user_settings.h via #include_next, then
 * undefines NO_FILESYSTEM.
 *
 * Why: the managed wolfSSL user_settings.h sets NO_FILESYSTEM (sensible for
 * an embedded build with no cert files). But wolfSSH gates its terminal
 * window-change API — wolfSSH_ChangeTerminalSize / SendChannelTerminalResize
 * — behind `#if defined(WOLFSSH_TERM) && !defined(NO_FILESYSTEM)`, even
 * though that path only sends an SSH CHANNEL_REQUEST "window-change" packet
 * (no actual filesystem use). Undefining NO_FILESYSTEM lets that API compile
 * so the Tab5 terminal can resize the server's pty (e.g. 80x33 to fill the
 * screen above the always-on keyboard). ESP-IDF provides newlib stdio over
 * VFS, so the now-enabled wolfSSL file paths still build; we simply don't
 * call them.
 *
 * Wired in the project root CMakeLists.txt via target_include_directories(
 * ... BEFORE PRIVATE) on the wolfssl/wolfssh/sshc targets.
 */
#include_next "user_settings.h"

#ifdef NO_FILESYSTEM
#undef NO_FILESYSTEM
#endif

/* Undefining NO_FILESYSTEM also un-gates wolfSSH's local-terminal code that
 * reads the host tty via termios (tcgetattr/VINTR/...), which ESP-IDF has no
 * headers for. The device is not a unix terminal — it sends an explicit
 * window-change with the size we choose — so keep that path off. The
 * window-change sender (SendChannelTerminalResize) is gated only on
 * !NO_FILESYSTEM, so it still compiles. */
#ifndef NO_TERMIOS
#define NO_TERMIOS
#endif
