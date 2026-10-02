/*
 * it_dialog_posix.c
 * -----------------
 * Feature 016 (issue #26): system file dialogs for the SDL backend on
 * Linux and other non-Apple POSIX systems. There is no system dialog
 * library to link against without new dependencies, so the desktop's
 * dialog helper runs as a child process: zenity (GNOME and most
 * desktops) or else kdialog (KDE). posix_spawnp with an argument vector
 * -- no shell, so paths need no quoting. While the helper runs, the
 * caller's idle callback keeps the tracker window responsive.
 *
 * Exit status 0 plus one line on stdout = chosen; anything else =
 * cancelled; neither helper on PATH = unavailable.
 */

#if defined(HAVE_SDL) && !defined(__APPLE__) && !defined(_WIN32)

/* posix_spawn, usleep, fcntl flags under a strict -std=c11 (Makefile) */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "it_screen.h"

extern char **environ;

static int on_path(const char *prog)
{
    const char *path = getenv("PATH");
    char buf[1024];

    while (path && *path) {
        const char *sep = strchr(path, ':');
        size_t n = sep ? (size_t)(sep - path) : strlen(path);
        if (n > 0 && n + strlen(prog) + 2 < sizeof(buf)) {
            memcpy(buf, path, n);
            buf[n] = '/';
            strcpy(buf + n + 1, prog);
            if (access(buf, X_OK) == 0)
                return 1;
        }
        path = sep ? sep + 1 : NULL;
    }
    return 0;
}

/* run argv, capture stdout; 1 = exit 0 with output */
static int run_helper(char *const argv[], char *out, size_t cap,
                      void (*idle)(void))
{
    int fd[2];
    pid_t pid;
    posix_spawn_file_actions_t fa;
    size_t n = 0;
    int status = 0, done = 0;

    if (pipe(fd) != 0)
        return -1;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_addclose(&fa, fd[0]);
    if (posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        close(fd[0]);
        close(fd[1]);
        return -1;
    }
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);

    /* non-blocking read + poll the child, keeping the window alive */
    {
        int fl = fcntl(fd[0], F_GETFL);
        fcntl(fd[0], F_SETFL, fl | O_NONBLOCK);
    }
    while (!done) {
        ssize_t k;
        char tmp[512];
        while ((k = read(fd[0], tmp, sizeof(tmp))) > 0) {
            size_t take = (size_t)k;
            if (n + take >= cap)
                take = cap - 1 - n;
            memcpy(out + n, tmp, take);
            n += take;
        }
        if (waitpid(pid, &status, WNOHANG) == pid)
            done = 1;
        else {
            if (idle)
                idle();
            usleep(20000);
        }
    }
    {                                   /* drain what is left */
        ssize_t k;
        char tmp[512];
        while ((k = read(fd[0], tmp, sizeof(tmp))) > 0) {
            size_t take = (size_t)k;
            if (n + take >= cap)
                take = cap - 1 - n;
            memcpy(out + n, tmp, take);
            n += take;
        }
    }
    close(fd[0]);
    out[n] = 0;
    {
        char *nl = strchr(out, '\n');
        if (nl)
            *nl = 0;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && out[0] ? 1 : 0;
}

/* "it;s3m" -> "*.it *.s3m" (zenity) / "*.it *.s3m" (kdialog) */
static void ext_glob(const char *exts, char *out, size_t cap)
{
    size_t n = 0;
    const char *p = exts;

    out[0] = 0;
    while (*p && n + 8 < cap) {
        if (n)
            out[n++] = ' ';
        out[n++] = '*';
        out[n++] = '.';
        while (*p && *p != ';' && n + 2 < cap)
            out[n++] = *p++;
        if (*p == ';')
            p++;
    }
    out[n] = 0;
}

int Dialog_Posix(const it_dialog_req_t *req, it_dialog_res_t *res,
                 void (*idle)(void))
{
    int save = req->kind == IT_DLG_SAVE_MODULE;
    int folder = req->kind == IT_DLG_PICK_FOLDER;
    char start[IT_DLG_PATH_MAX + 64], glob[256], filt1[320], filt2[320];
    char title[64], out[IT_DLG_PATH_MAX + 2];
    char *argv[12];
    int argc = 0, r;
    char cwd[IT_DLG_PATH_MAX];

    res->status = IT_DLG_UNAVAILABLE;
    if (req->start_dir && *req->start_dir)
        snprintf(cwd, sizeof(cwd), "%s", req->start_dir);
    else if (!getcwd(cwd, sizeof(cwd)))
        snprintf(cwd, sizeof(cwd), ".");
    snprintf(title, sizeof(title), "%s", Screen_DialogTitle(req->kind));

    if (on_path("zenity")) {
        argv[argc++] = "zenity";
        argv[argc++] = "--file-selection";
        argv[argc++] = "--title";
        argv[argc++] = title;
        if (folder)
            argv[argc++] = "--directory";
        if (save) {
            argv[argc++] = "--save";
            snprintf(start, sizeof(start), "--filename=%s/%s", cwd,
                     req->suggest_name ? req->suggest_name : "");
            snprintf(filt1, sizeof(filt1),
                     "--file-filter=Impulse Tracker (*.it) | *.it *.IT");
            snprintf(filt2, sizeof(filt2),
                     "--file-filter=Scream Tracker 3 (*.s3m) | *.s3m *.S3M");
            argv[argc++] = start;
            argv[argc++] = req->save_format == 1 ? filt2 : filt1;
            argv[argc++] = req->save_format == 1 ? filt1 : filt2;
        } else {
            snprintf(start, sizeof(start), "--filename=%s/", cwd);
            argv[argc++] = start;
            if (!folder) {
                ext_glob(Screen_DialogExts(req->kind), glob, sizeof(glob));
                snprintf(filt1, sizeof(filt1), "--file-filter=%s | %s",
                         title, glob);
                snprintf(filt2, sizeof(filt2), "--file-filter=All files | *");
                argv[argc++] = filt1;
                argv[argc++] = filt2;
            }
        }
    } else if (on_path("kdialog")) {
        argv[argc++] = "kdialog";
        argv[argc++] = "--title";
        argv[argc++] = title;
        if (folder) {
            argv[argc++] = "--getexistingdirectory";
            argv[argc++] = cwd;
        } else if (save) {
            argv[argc++] = "--getsavefilename";
            snprintf(start, sizeof(start), "%s/%s", cwd,
                     req->suggest_name ? req->suggest_name : "");
            argv[argc++] = start;
            snprintf(filt1, sizeof(filt1),
                     "Impulse Tracker (*.it *.IT)\nScream Tracker 3 (*.s3m *.S3M)");
            argv[argc++] = filt1;
        } else {
            argv[argc++] = "--getopenfilename";
            argv[argc++] = cwd;
            ext_glob(Screen_DialogExts(req->kind), glob, sizeof(glob));
            snprintf(filt1, sizeof(filt1), "%s (%s)\nAll files (*)",
                     title, glob);
            argv[argc++] = filt1;
        }
    } else {
        return res->status;             /* neither helper installed */
    }
    argv[argc] = NULL;

    r = run_helper(argv, out, sizeof(out), idle);
    if (r < 0) {
        res->status = IT_DLG_UNAVAILABLE;
    } else if (r == 0) {
        res->status = IT_DLG_CANCELLED;
    } else if (strlen(out) >= sizeof(res->path) - 1) {
        res->status = IT_DLG_REJECTED;
        res->reason = "Path too long";
    } else {
        memcpy(res->path, out, strlen(out) + 1);
        res->lossy = Screen_Utf8ToCP437Display(res->path, res->display,
                                               sizeof(res->display));
        if (save)
            res->save_format = Screen_SaveFormatFromName(res->path);
        res->status = IT_DLG_CHOSEN;
    }
    return res->status;
}

#else
typedef int it_dialog_posix_unused;     /* ISO C: no empty translation unit */
#endif
