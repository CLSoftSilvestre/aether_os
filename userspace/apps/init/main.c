/*
 * aesh — AetherOS Shell (PID 1)
 * File: userspace/apps/init/main.c
 *
 * A minimal interactive shell running in EL0. Reads commands from stdin
 * (UART via sys_read), interprets built-ins, loops forever.
 *
 * Built-in commands:
 *   help    — list commands
 *   echo    — print arguments
 *   ls      — list initrd files
 *   clear   — clear the terminal
 *   uname   — print OS name
 *   exit    — terminate shell (sys_exit)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys.h>

#define PROMPT    "aesh> "
#define LINE_MAX  256
#define ARGV_MAX   16

/* ── Helpers ──────────────────────────────────────────────────────────── */

static void print_banner(void)
{
    printf("\r\n");
    printf("  ___       _   _                  ___  ____\r\n");
    printf(" / _ \\ ___ | |_| |__   ___ _ __   / _ \\/ ___|\r\n");
    printf("| | | / _ \\| __| '_ \\ / _ \\ '__| | | | \\___ \\\r\n");
    printf("| |_| |  __/| |_| | | |  __/ |    | |_| |___) |\r\n");
    printf(" \\___/ \\___| \\__|_| |_|\\___|_|     \\___/|____/\r\n");
    printf("\r\n");
    printf("  AetherOS -- Phase 3.2  |  type 'help' for commands\r\n");
    printf("\r\n");
}

/* Split line into argv[], return argc.  Modifies line in-place. */
static int parse_args(char *line, char **argv, int maxargs)
{
    int argc = 0;
    char *tok = strtok(line, " \t");
    while (tok && argc < maxargs - 1) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    argv[argc] = NULL;
    return argc;
}

/* ── Built-in commands ────────────────────────────────────────────────── */

static void cmd_help(void)
{
    printf("Built-in commands:\r\n");
    printf("  help           show this message\r\n");
    printf("  echo [args]    print arguments\r\n");
    printf("  ls             list files in initrd\r\n");
    printf("  clear          clear the screen\r\n");
    printf("  uname          print OS information\r\n");
    printf("  exit [code]    exit the shell\r\n");
}

static void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) putchar(' ');
        printf("%s", argv[i]);
    }
    printf("\r\n");
}

static void cmd_ls(void)
{
    char buf[2048];
    long n = sys_initrd_ls(buf, sizeof(buf));
    if (n < 0) {
        printf("ls: failed\r\n");
        return;
    }
    for (long i = 0; i < n; i++) {
        if (buf[i] == '\n') printf("\r\n");
        else putchar(buf[i]);
    }
}

static void cmd_clear(void)
{
    printf("\033[2J\033[H");
}

static void cmd_uname(void)
{
    printf("AetherOS  aarch64  Phase 3.2  QEMU virt (Cortex-A76)\r\n");
}

/* ── Main shell loop ──────────────────────────────────────────────────── */

int main(void)
{
    print_banner();

    char line[LINE_MAX];
    char *argv[ARGV_MAX];

    for (;;) {
        printf(PROMPT);

        int n = readline(line, LINE_MAX);
        if (n == 0) continue;

        /* Trim trailing whitespace */
        while (n > 0 && (line[n-1] == ' ' || line[n-1] == '\t'))
            line[--n] = '\0';
        if (n == 0) continue;

        int argc = parse_args(line, argv, ARGV_MAX);
        if (argc == 0) continue;

        const char *cmd = argv[0];

        if (strcmp(cmd, "help") == 0) {
            cmd_help();
        } else if (strcmp(cmd, "echo") == 0) {
            cmd_echo(argc, argv);
        } else if (strcmp(cmd, "ls") == 0) {
            cmd_ls();
        } else if (strcmp(cmd, "clear") == 0) {
            cmd_clear();
        } else if (strcmp(cmd, "uname") == 0) {
            cmd_uname();
        } else if (strcmp(cmd, "exit") == 0) {
            int code = (argc > 1) ? atoi(argv[1]) : 0;
            printf("Goodbye!\r\n");
            exit(code);
        } else {
            printf("aesh: %s: command not found\r\n", cmd);
        }
    }

    return 0;   /* unreachable */
}
