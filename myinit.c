#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LOG_FILE "/tmp/myinit.log"
#define PID_FILE "/tmp/myinit.pid"
#define MAX_LINE 4096

struct child_cfg {
    char *line;
    char **argv;
    int argc;
    char *stdin_path;
    char *stdout_path;
    pid_t pid;
};

struct config {
    struct child_cfg *items;
    size_t count;
};

static volatile sig_atomic_t hup_requested = 0;
static volatile sig_atomic_t stop_requested = 0;
static int log_fd = -1;
static char *config_path = NULL;

static void die(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

static void log_msg(const char *fmt, ...)
{
    char prefix[64];
    time_t now;
    struct tm tm_buf;
    va_list ap;

    if (log_fd < 0) {
        return;
    }

    now = time(NULL);
    if (localtime_r(&now, &tm_buf) != NULL) {
        strftime(prefix, sizeof(prefix), "%Y-%m-%d %H:%M:%S", &tm_buf);
        dprintf(log_fd, "[%s] ", prefix);
    }

    va_start(ap, fmt);
    vdprintf(log_fd, fmt, ap);
    va_end(ap);
    dprintf(log_fd, "\n");
}

static void handle_hup(int sig)
{
    (void)sig;
    hup_requested = 1;
}

static void handle_term(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static int is_absolute_path(const char *path)
{
    return path != NULL && path[0] == '/';
}

static char *xstrdup(const char *s)
{
    char *copy = strdup(s);
    if (copy == NULL) {
        die("strdup failed: %s", strerror(errno));
    }
    return copy;
}

static void free_child_cfg(struct child_cfg *cfg)
{
    int i;

    if (cfg == NULL) {
        return;
    }
    free(cfg->line);
    for (i = 0; i < cfg->argc; i++) {
        free(cfg->argv[i]);
    }
    free(cfg->argv);
    free(cfg->stdin_path);
    free(cfg->stdout_path);
    memset(cfg, 0, sizeof(*cfg));
}

static void free_config(struct config *cfg)
{
    size_t i;

    if (cfg == NULL) {
        return;
    }
    for (i = 0; i < cfg->count; i++) {
        free_child_cfg(&cfg->items[i]);
    }
    free(cfg->items);
    cfg->items = NULL;
    cfg->count = 0;
}

static int split_line(char *line, char ***tokens_out)
{
    int count = 0;
    int capacity = 8;
    char **tokens;
    char *p = line;

    tokens = calloc((size_t)capacity, sizeof(*tokens));
    if (tokens == NULL) {
        die("calloc failed: %s", strerror(errno));
    }

    while (*p != '\0') {
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0' || *p == '#') {
            break;
        }
        if (count == capacity) {
            char **new_tokens;
            capacity *= 2;
            new_tokens = realloc(tokens, (size_t)capacity * sizeof(*tokens));
            if (new_tokens == NULL) {
                free(tokens);
                die("realloc failed: %s", strerror(errno));
            }
            tokens = new_tokens;
        }
        tokens[count++] = p;
        while (*p != '\0' && !isspace((unsigned char)*p)) {
            p++;
        }
        if (*p != '\0') {
            *p++ = '\0';
        }
    }

    *tokens_out = tokens;
    return count;
}

static void add_config_item(struct config *cfg, struct child_cfg *item)
{
    struct child_cfg *new_items;

    new_items = realloc(cfg->items, (cfg->count + 1) * sizeof(*cfg->items));
    if (new_items == NULL) {
        die("realloc failed: %s", strerror(errno));
    }
    cfg->items = new_items;
    cfg->items[cfg->count] = *item;
    cfg->count++;
}

static struct config read_config_file(const char *path)
{
    FILE *fp;
    struct config cfg = {0};
    char line[MAX_LINE];
    int line_no = 0;

    if (!is_absolute_path(path)) {
        die("configuration file path must be absolute: %s", path);
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        die("cannot open configuration file %s: %s", path, strerror(errno));
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char original[MAX_LINE];
        char *nl;
        char **tokens = NULL;
        int token_count;
        int i;
        struct child_cfg item = {0};

        line_no++;
        nl = strchr(line, '\n');
        if (nl != NULL) {
            *nl = '\0';
        } else if (!feof(fp)) {
            fclose(fp);
            die("line %d in %s is too long", line_no, path);
        }

        strncpy(original, line, sizeof(original));
        original[sizeof(original) - 1] = '\0';
        token_count = split_line(line, &tokens);
        if (token_count == 0) {
            free(tokens);
            continue;
        }
        if (token_count < 3) {
            free(tokens);
            fclose(fp);
            die("line %d: expected command, stdin and stdout paths", line_no);
        }
        if (!is_absolute_path(tokens[0]) ||
            !is_absolute_path(tokens[token_count - 2]) ||
            !is_absolute_path(tokens[token_count - 1])) {
            free(tokens);
            fclose(fp);
            die("line %d: executable, stdin and stdout paths must be absolute", line_no);
        }

        item.line = xstrdup(original);
        item.argc = token_count - 2;
        item.argv = calloc((size_t)item.argc + 1, sizeof(*item.argv));
        if (item.argv == NULL) {
            free(tokens);
            fclose(fp);
            die("calloc failed: %s", strerror(errno));
        }
        for (i = 0; i < item.argc; i++) {
            item.argv[i] = xstrdup(tokens[i]);
        }
        item.argv[item.argc] = NULL;
        item.stdin_path = xstrdup(tokens[token_count - 2]);
        item.stdout_path = xstrdup(tokens[token_count - 1]);
        item.pid = 0;
        add_config_item(&cfg, &item);
        free(tokens);
    }

    if (ferror(fp)) {
        fclose(fp);
        die("cannot read configuration file %s: %s", path, strerror(errno));
    }
    fclose(fp);
    return cfg;
}

static void close_all_fds_except(int keep_fd)
{
    long max_fd;
    int fd;

    max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0) {
        max_fd = 1024;
    }
    for (fd = 0; fd < max_fd; fd++) {
        if (fd != keep_fd) {
            close(fd);
        }
    }
}

static void close_extra_fds_except_log(void)
{
    long max_fd;
    int fd;

    max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0) {
        max_fd = 1024;
    }
    for (fd = 3; fd < max_fd; fd++) {
        if (fd != log_fd) {
            close(fd);
        }
    }
}

static void redirect_stdio(const struct child_cfg *cfg)
{
    int in_fd;
    int out_fd;
    int null_fd;

    in_fd = open(cfg->stdin_path, O_RDONLY);
    if (in_fd < 0) {
        log_msg("child setup failed: cannot open stdin %s: %s",
                cfg->stdin_path, strerror(errno));
        _exit(127);
    }
    out_fd = open(cfg->stdout_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (out_fd < 0) {
        log_msg("child setup failed: cannot open stdout %s: %s",
                cfg->stdout_path, strerror(errno));
        close(in_fd);
        _exit(127);
    }
    null_fd = open("/dev/null", O_WRONLY);
    if (null_fd < 0) {
        log_msg("child setup failed: cannot open /dev/null: %s", strerror(errno));
        close(in_fd);
        close(out_fd);
        _exit(127);
    }

    if (dup2(in_fd, STDIN_FILENO) < 0 ||
        dup2(out_fd, STDOUT_FILENO) < 0 ||
        dup2(null_fd, STDERR_FILENO) < 0) {
        log_msg("child setup failed: dup2 failed: %s", strerror(errno));
        _exit(127);
    }
    close(in_fd);
    close(out_fd);
    close(null_fd);
}

static void start_child(struct child_cfg *cfg, size_t index)
{
    pid_t pid;

    pid = fork();
    if (pid < 0) {
        log_msg("fork failed for config line %zu: %s", index + 1, strerror(errno));
        return;
    }
    if (pid == 0) {
        signal(SIGHUP, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        redirect_stdio(cfg);
        if (log_fd >= 0) {
            fcntl(log_fd, F_SETFD, FD_CLOEXEC);
        }
        close_extra_fds_except_log();
        execv(cfg->argv[0], cfg->argv);
        log_msg("exec failed for %s: %s", cfg->argv[0], strerror(errno));
        _exit(127);
    }

    cfg->pid = pid;
    log_msg("START line=%zu pid=%ld command=%s", index + 1, (long)pid, cfg->line);
}

static void start_all_children(struct config *cfg)
{
    size_t i;

    for (i = 0; i < cfg->count; i++) {
        start_child(&cfg->items[i], i);
    }
}

static void log_child_status(pid_t pid, int status)
{
    if (WIFEXITED(status)) {
        log_msg("EXIT pid=%ld code=%d", (long)pid, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        log_msg("EXIT pid=%ld signal=%d", (long)pid, WTERMSIG(status));
    } else {
        log_msg("EXIT pid=%ld status=%d", (long)pid, status);
    }
}

static ssize_t find_child_by_pid(const struct config *cfg, pid_t pid)
{
    size_t i;

    for (i = 0; i < cfg->count; i++) {
        if (cfg->items[i].pid == pid) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static void terminate_all_children(struct config *cfg)
{
    size_t i;

    for (i = 0; i < cfg->count; i++) {
        if (cfg->items[i].pid > 0) {
            log_msg("STOP line=%zu pid=%ld", i + 1, (long)cfg->items[i].pid);
            if (kill(cfg->items[i].pid, SIGTERM) < 0 && errno != ESRCH) {
                log_msg("kill failed for pid=%ld: %s",
                        (long)cfg->items[i].pid, strerror(errno));
            }
        }
    }

    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, 0);

        if (pid > 0) {
            ssize_t index = find_child_by_pid(cfg, pid);
            log_child_status(pid, status);
            if (index >= 0) {
                cfg->items[index].pid = 0;
            }
            continue;
        }
        if (pid < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

static void reopen_log(void)
{
    if (log_fd >= 0) {
        close(log_fd);
    }
    log_fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        _exit(EXIT_FAILURE);
    }
}

static void daemonize_process(void)
{
    pid_t pid;

    pid = fork();
    if (pid < 0) {
        die("first fork failed: %s", strerror(errno));
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        _exit(EXIT_FAILURE);
    }

    signal(SIGHUP, SIG_IGN);
    pid = fork();
    if (pid < 0) {
        _exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (chdir("/") < 0) {
        _exit(EXIT_FAILURE);
    }
    umask(0);
    close_all_fds_except(-1);
    reopen_log();
}

static void write_pid_file(void)
{
    int fd;

    fd = open(PID_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_msg("cannot write pid file %s: %s", PID_FILE, strerror(errno));
        return;
    }
    dprintf(fd, "%ld\n", (long)getpid());
    close(fd);
}

static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handle_hup;
    if (sigaction(SIGHUP, &sa, NULL) < 0) {
        log_msg("sigaction SIGHUP failed: %s", strerror(errno));
        _exit(EXIT_FAILURE);
    }

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handle_term;
    if (sigaction(SIGTERM, &sa, NULL) < 0 || sigaction(SIGINT, &sa, NULL) < 0) {
        log_msg("sigaction SIGTERM/SIGINT failed: %s", strerror(errno));
        _exit(EXIT_FAILURE);
    }
}

static void reload_config(struct config *cfg)
{
    struct config new_cfg;

    log_msg("SIGHUP received: reloading configuration");
    terminate_all_children(cfg);
    free_config(cfg);
    new_cfg = read_config_file(config_path);
    *cfg = new_cfg;
    log_msg("configuration loaded: %zu process(es)", cfg->count);
    start_all_children(cfg);
}

static void monitor_loop(struct config *cfg)
{
    while (!stop_requested) {
        int status;
        pid_t pid;

        if (hup_requested) {
            hup_requested = 0;
            reload_config(cfg);
            continue;
        }

        pid = waitpid(-1, &status, 0);
        if (pid > 0) {
            ssize_t index = find_child_by_pid(cfg, pid);
            log_child_status(pid, status);
            if (index >= 0 && !stop_requested && !hup_requested) {
                cfg->items[index].pid = 0;
                log_msg("RESTART line=%ld old_pid=%ld", (long)(index + 1), (long)pid);
                start_child(&cfg->items[index], (size_t)index);
            }
        } else if (pid < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                sleep(1);
                continue;
            }
            log_msg("waitpid failed: %s", strerror(errno));
        }
    }
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s -c /absolute/path/to/config\n", prog);
}

int main(int argc, char **argv)
{
    int opt;
    struct config cfg;

    while ((opt = getopt(argc, argv, "c:h")) != -1) {
        switch (opt) {
        case 'c':
            config_path = xstrdup(optarg);
            break;
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (config_path == NULL && optind < argc) {
        config_path = xstrdup(argv[optind]);
    }
    if (config_path == NULL || !is_absolute_path(config_path)) {
        usage(argv[0]);
        die("configuration file path must be absolute");
    }

    cfg = read_config_file(config_path);
    daemonize_process();
    install_signal_handlers();
    write_pid_file();
    log_msg("myinit started pid=%ld config=%s", (long)getpid(), config_path);
    log_msg("configuration loaded: %zu process(es)", cfg.count);
    start_all_children(&cfg);
    monitor_loop(&cfg);
    log_msg("myinit stopping");
    terminate_all_children(&cfg);
    unlink(PID_FILE);
    free_config(&cfg);
    free(config_path);
    log_msg("myinit stopped");
    close(log_fd);
    return EXIT_SUCCESS;
}
