#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Platform-specific includes and definitions
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define mkdir(path, mode) _mkdir(path)
#define rmdir _rmdir
#define unlink _unlink
// Windows doesn't have WEXITSTATUS
#define WEXITSTATUS(status) (((status) & 0xff00) >> 8)
#endif

// Color definitions
#define COLOR_RESET "\033[0m"
#define COLOR_GREEN "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_CYAN "\033[36m"
#define COLOR_WHITE "\033[37m"
#define COLOR_RED "\033[31m"

// Configuration constants
#define KAISHAKU_DIR ".git/kaishaku"
#define CONFIG_FILE ".git/config"
#define DEFAULT_BUFFER_SIZE 512
#define MAX_PATH_LENGTH 1024

// Safe path buffer functions
#define SESSION_DIR(session) (safe_path_join(KAISHAKU_DIR, session))
#define SESSION_FILE(session) (safe_path_join(SESSION_DIR(session), "session"))
#define HEAD_FILE(session) (safe_path_join(SESSION_DIR(session), "head"))
#define ACTIVE_FILE KAISHAKU_DIR "/.active"
#define SESSION_TIME_FILE(session) (safe_path_join(SESSION_DIR(session), "time"))
#define SESSION_DESC_FILE(session) (safe_path_join(SESSION_DIR(session), "desc"))

// Global error state
char error_message[DEFAULT_BUFFER_SIZE];

#define COMMAND_LIST(X, ...)      \
    X(checkout, argv2, argv3)     \
    X(switch, argv2)              \
    X(branch, argv2)              \
    X(save, argv2)                \
    X(exit, argv2)                \
    X(status)                     \
    X(list)                       \
    X(clean, argv2)               \
    X(config, argc - 2, argv + 2) \
    X(recover, argv2)             \
    X(rename, argv2, argv3)       \
    X(abort, argv2)

// Command string
// #define COMMAND_STRING " checkout switch branch save exit status list clean config recover rename
// abort "

#define CMD_NAME(c, ...) " " #c

#define CMD_ENUM(c, ...) CMD_OFFSET_##c = 0,
// #pragma GCC diagnostic push
// #pragma GCC diagnostic ignored "-Wpedantic"

#define CMD_CASE(c, ...)      \
    case CMD(#c):             \
        cmd_##c(__VA_ARGS__); \
        break;
// #pragma GCC diagnostic pop

#define COMMAND_STRING_LITERAL COMMAND_LIST(CMD_NAME) " "

static const char COMMAND_STRING[] = COMMAND_STRING_LITERAL;

#define CMD(c) ((int)(strstr(COMMAND_STRING, " " c " ") - COMMAND_STRING))

// All command enums with zero value
enum { COMMAND_LIST(CMD_ENUM) };

// Match command to offset using strstr at runtime
int get_command_offset(const char* cmd) {
    char search[32];
    snprintf(search, sizeof(search), " %s ", cmd);
    char* found = strstr(COMMAND_STRING, search);
    return found ? (int)(found - COMMAND_STRING) : -1;
}

// Function declarations
void usage(void);
int file_exists(const char* filename);
char* safe_path_join(const char* dir, const char* file);
void ensure_directory_exists(const char* dir);
int write_to_file(const char* path, const char* content);
char* read_from_file(const char* path);
int execute_git_command(const char* cmd, char* output, size_t output_size);
void cmd_checkout(const char* session, const char* commit);
void cmd_switch(const char* session);
void cmd_branch(const char* branch_name);
void cmd_save(const char* branch_name);
void cmd_exit(const char* option);
void cmd_status(void);
void cmd_list(void);
void cmd_clean(const char* session);
void cmd_config(int argc, char* argv[]);
void load_config(void);
void cmd_recover(const char* session);
void cmd_rename(const char* old_name, const char* new_name);
void cmd_abort(const char* session);
void update_timestamp(const char* session);
char* get_session_time(const char* session);

// Configuration options with defaults
struct {
    int confirm_exit;
    int auto_stash;
    int auto_save;  // Add auto_save configuration
} config = {.confirm_exit = 1, .auto_stash = 0, .auto_save = 0};

// Safe path joining function

__attribute__((optimize("O2")))  // Avoid -O3 false positive: snprintf() input may alias static buffer (safe).
char* safe_path_join(const char* dir, const char* file) {
    static char path_buffer[MAX_PATH_LENGTH];

    if (snprintf(path_buffer, sizeof(path_buffer), "%s/%s", dir, file) >=
        (int)sizeof(path_buffer)) {
        fprintf(stderr, "Error: Path too long: %s/%s\n", dir, file);
        exit(EXIT_FAILURE);
    }

    return path_buffer;
}

void usage(void) {
    printf(
        "%skaishaku - a minimal tool for safe Git experimentation via detached HEAD sessions%s\n\n",
        COLOR_CYAN, COLOR_RESET);
    printf("%sUsage:%s\n", COLOR_CYAN, COLOR_RESET);
    printf("  %skaishaku checkout%s <session> [<commit>]  Start a new session from commit\n",
           COLOR_YELLOW, COLOR_RESET);
    printf("  %skaishaku switch%s <session>              Switch to an existing session\n",
           COLOR_YELLOW, COLOR_RESET);
    printf("  %skaishaku branch%s <name>                 Create a branch from current session\n",
           COLOR_YELLOW, COLOR_RESET);
    printf("  %skaishaku save%s <name>                   Save session changes to original branch\n",
           COLOR_YELLOW, COLOR_RESET);
    printf("  %skaishaku status%s                        Show current session status\n",
           COLOR_YELLOW, COLOR_RESET);
    printf("  %skaishaku list%s                          List all sessions\n", COLOR_YELLOW,
           COLOR_RESET);
    printf("  %skaishaku clean%s [<session>]             Remove session(s)\n", COLOR_YELLOW,
           COLOR_RESET);
    printf(
        "  %skaishaku exit%s [--force | --keep | --save | --no-save]  Exit session and return to "
        "original branch\n",
        COLOR_YELLOW, COLOR_RESET);
    printf("  %skaishaku config%s [get|set] <key> [<value>]  Get or set configuration\n",
           COLOR_YELLOW, COLOR_RESET);
    printf("  %skaishaku recover%s <session>             Recover a corrupted session\n",
           COLOR_YELLOW, COLOR_RESET);
    printf("  %skaishaku rename%s <old> <new>            Rename a session\n", COLOR_YELLOW,
           COLOR_RESET);
    printf("  %skaishaku abort%s [<session>]             Abort and clean up a session\n",
           COLOR_YELLOW, COLOR_RESET);
    printf("  %skaishaku help%s                          Show this help message\n", COLOR_YELLOW,
           COLOR_RESET);

    printf("\n%sConfiguration options:%s\n", COLOR_CYAN, COLOR_RESET);
    printf("  %sconfirm.exit%s    Whether to confirm before exiting (0/1)\n", COLOR_YELLOW,
           COLOR_RESET);
    printf("  %sauto.stash%s      Whether to auto-stash changes on exit (0/1)\n", COLOR_YELLOW,
           COLOR_RESET);
    printf("  %sauto.save%s       Whether to auto-save changes on exit (0/1)\n\n", COLOR_YELLOW,
           COLOR_RESET);
    exit(0);
}

int file_exists(const char* filename) {
    return access(filename, F_OK) != -1;
}

void ensure_directory_exists(const char* dir) {
    struct stat st;

    if (stat(dir, &st) == -1) {
        if (errno == ENOENT) {
            // Directory doesn't exist, create it
            if (mkdir(dir, 0755) == -1) {
                fprintf(stderr, "Error: Failed to create directory %s: %s\n", dir, strerror(errno));
                exit(EXIT_FAILURE);
            }
        } else {
            fprintf(stderr, "Error: Cannot access directory %s: %s\n", dir, strerror(errno));
            exit(EXIT_FAILURE);
        }
    } else if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: %s exists but is not a directory\n", dir);
        exit(EXIT_FAILURE);
    }
}

int write_to_file(const char* path, const char* content) {
    FILE* fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "Error: Failed to write to %s: %s\n", path, strerror(errno));
        return 0;
    }

    fprintf(fp, "%s\n", content);
    if (ferror(fp)) {
        fprintf(stderr, "Error: Failed to write to %s: %s\n", path, strerror(errno));
        fclose(fp);
        return 0;
    }

    fclose(fp);
    return 1;
}

char* read_from_file(const char* path) {
    static char buffer[DEFAULT_BUFFER_SIZE];

    FILE* fp = fopen(path, "r");
    if (!fp)
        return NULL;

    if (!fgets(buffer, sizeof(buffer), fp)) {
        fclose(fp);
        return NULL;
    }

    fclose(fp);

    // Remove trailing newline
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n') {
        buffer[len - 1] = '\0';
    }

    return buffer;
}

int execute_git_command(const char* cmd, char* output, size_t output_size) {
    FILE* fp = popen(cmd, "r");
    if (!fp) {
        snprintf(error_message, sizeof(error_message), "Failed to execute command: %s", cmd);
        return 0;
    }

    if (output && output_size > 0) {
        if (!fgets(output, output_size, fp)) {
            pclose(fp);
            snprintf(error_message, sizeof(error_message), "Command returned no output: %s", cmd);
            return 0;
        }

        // Remove trailing newline
        size_t len = strlen(output);
        if (len > 0 && output[len - 1] == '\n') {
            output[len - 1] = '\0';
        }
    }

    int status = pclose(fp);
    if (status == -1 || WEXITSTATUS(status) != 0) {
        snprintf(error_message, sizeof(error_message), "Command failed with status %d: %s",
                 WEXITSTATUS(status), cmd);
        return 0;
    }

    return 1;
}

void cmd_checkout(const char* session, const char* commit) {
    if (!session)
        usage();
    ensure_directory_exists(KAISHAKU_DIR);

    char* session_dir = SESSION_DIR(session);
    ensure_directory_exists(session_dir);

    char current_branch[DEFAULT_BUFFER_SIZE];
    if (!execute_git_command("git rev-parse --abbrev-ref HEAD", current_branch,
                             sizeof(current_branch))) {
        fprintf(stderr, "Error: %s\n", error_message);
        exit(EXIT_FAILURE);
    }

    if (!write_to_file(SESSION_FILE(session), current_branch)) {
        exit(EXIT_FAILURE);
    }

    char head_hash[DEFAULT_BUFFER_SIZE];
    if (!commit) {
        if (!execute_git_command("git rev-parse HEAD", head_hash, sizeof(head_hash))) {
            fprintf(stderr, "Error: %s\n", error_message);
            exit(EXIT_FAILURE);
        }
        commit = head_hash;
    }

    if (!write_to_file(HEAD_FILE(session), commit)) {
        exit(EXIT_FAILURE);
    }

    if (!write_to_file(ACTIVE_FILE, session)) {
        exit(EXIT_FAILURE);
    }

    update_timestamp(session);  // Update timestamp when creating session

    char git_cmd[DEFAULT_BUFFER_SIZE + 27];
    snprintf(git_cmd, sizeof(git_cmd), "git checkout %s --detach", commit);

    if (!execute_git_command(git_cmd, NULL, 0)) {
        fprintf(stderr, "Error: %s\n", error_message);
        exit(EXIT_FAILURE);
    }

    printf("%sSession '%s' started at %s%s\n", COLOR_GREEN, session, commit, COLOR_RESET);
}

void cmd_switch(const char* session) {
    if (!session)
        usage();

    ensure_directory_exists(KAISHAKU_DIR);

    const char* target_head = read_from_file(HEAD_FILE(session));
    if (!target_head) {
        fprintf(stderr, "Error: Session '%s' not found.\n", session);
        exit(EXIT_FAILURE);
    }

    if (!write_to_file(ACTIVE_FILE, session)) {
        exit(EXIT_FAILURE);
    }

    update_timestamp(session);  // Update timestamp when switching to session

    char git_cmd[DEFAULT_BUFFER_SIZE];
    snprintf(git_cmd, sizeof(git_cmd), "git checkout %s --detach", target_head);

    if (!execute_git_command(git_cmd, NULL, 0)) {
        fprintf(stderr, "Error: %s\n", error_message);
        exit(EXIT_FAILURE);
    }

    printf("%sSwitched to session '%s'%s\n", COLOR_GREEN, session, COLOR_RESET);
}

void cmd_branch(const char* branch_name) {
    if (!branch_name)
        usage();

    if (!file_exists(ACTIVE_FILE)) {
        fprintf(stderr, "Error: No active kaishaku session.\n");
        exit(EXIT_FAILURE);
    }

    char git_cmd[DEFAULT_BUFFER_SIZE];
    snprintf(git_cmd, sizeof(git_cmd), "git checkout -b %s", branch_name);

    if (!execute_git_command(git_cmd, NULL, 0)) {
        fprintf(stderr, "Error: %s\n", error_message);
        exit(EXIT_FAILURE);
    }

    const char* session = read_from_file(ACTIVE_FILE);
    if (!session) {
        fprintf(stderr, "Error: Failed to read active session.\n");
        exit(EXIT_FAILURE);
    }

    if (!write_to_file(HEAD_FILE(session), "HEAD")) {
        exit(EXIT_FAILURE);
    }

    printf("%sCreated branch '%s' from session '%s'%s\n", COLOR_GREEN, branch_name, session,
           COLOR_RESET);
}

void cmd_save(const char* branch_name) {
    if (!branch_name)
        usage();

    if (!file_exists(ACTIVE_FILE)) {
        fprintf(stderr, "Error: No active kaishaku session\n");
        exit(EXIT_FAILURE);
    }

    char* session = read_from_file(ACTIVE_FILE);
    if (!session) {
        fprintf(stderr, "Error: Failed to read active session\n");
        exit(EXIT_FAILURE);
    }

    char* original_branch = read_from_file(SESSION_FILE(session));
    if (!original_branch) {
        fprintf(stderr, "Error: Original branch not found for session '%s'\n", session);
        exit(EXIT_FAILURE);
    }

    // Create a temporary branch from current session
    char cmd[DEFAULT_BUFFER_SIZE];
    snprintf(cmd, sizeof(cmd), "git checkout -b %s", branch_name);
    if (!execute_git_command(cmd, NULL, 0)) {
        fprintf(stderr, "Error: Failed to create branch: %s\n", error_message);
        exit(EXIT_FAILURE);
    }

    // Switch back to original branch
    snprintf(cmd, sizeof(cmd), "git checkout %s", original_branch);
    if (!execute_git_command(cmd, NULL, 0)) {
        fprintf(stderr, "Error: Failed to return to original branch: %s\n", error_message);
        exit(EXIT_FAILURE);
    }

    // Merge the temporary branch
    snprintf(cmd, sizeof(cmd), "git merge %s", branch_name);
    if (!execute_git_command(cmd, NULL, 0)) {
        // If merge fails, clean up and exit
        snprintf(cmd, sizeof(cmd), "git checkout %s", branch_name);
        execute_git_command(cmd, NULL, 0);
        snprintf(cmd, sizeof(cmd), "git branch -D %s", branch_name);
        execute_git_command(cmd, NULL, 0);
        fprintf(stderr, "Error: Failed to merge changes. Please resolve conflicts manually.\n");
        exit(EXIT_FAILURE);
    }

    // Delete the temporary branch
    snprintf(cmd, sizeof(cmd), "git branch -D %s", branch_name);
    if (!execute_git_command(cmd, NULL, 0)) {
        fprintf(stderr, "Warning: Failed to delete temporary branch '%s'\n", branch_name);
    }

    write_to_file(HEAD_FILE(session), "HEAD");
    update_timestamp(session);  // Update timestamp when saving changes
    printf("%sSuccessfully saved changes from session '%s' to branch '%s'%s\n", COLOR_GREEN,
           session, original_branch, COLOR_RESET);
}

void cmd_exit(const char* option) {
    if (!file_exists(ACTIVE_FILE)) {
        fprintf(stderr, "Error: No active kaishaku session.\n");
        exit(EXIT_FAILURE);
    }

    const char* session = read_from_file(ACTIVE_FILE);
    if (!session) {
        fprintf(stderr, "Error: Failed to read active session.\n");
        exit(EXIT_FAILURE);
    }

    const char* original_branch = read_from_file(SESSION_FILE(session));
    if (!original_branch) {
        fprintf(stderr, "Error: Original branch not found for session '%s'\n", session);
        exit(EXIT_FAILURE);
    }

    int force = option && strcmp(option, "--force") == 0;
    int keep = option && strcmp(option, "--keep") == 0;
    int save = option && strcmp(option, "--save") == 0;
    int no_save = option && strcmp(option, "--no-save") == 0;

    // Check if we should automatically save changes
    if (config.auto_save && !no_save) {
        save = 1;
    }

    // Check if we should automatically stash changes (only if not saving)
    if (config.auto_stash && !save && !force) {
        keep = 1;
    }

    // Check if there are actually any changes
    int has_changes = 0;
    char changes_output[DEFAULT_BUFFER_SIZE] = "";
    if (execute_git_command("git status --porcelain", changes_output, sizeof(changes_output))) {
        has_changes = changes_output[0] != '\0';
    }

    // Confirm before discarding changes if needed
    if (!force && !keep && !save && config.confirm_exit && has_changes) {
        printf("Discard uncommitted changes and exit? (y/N): ");
        char c = getchar();
        if (c != 'y' && c != 'Y') {
            puts("Aborted.");
            exit(0);
        }
    }

    // Handle changes based on configuration and options
    if (has_changes) {
        if (save) {
            // Save changes before exiting
            char commit_msg[DEFAULT_BUFFER_SIZE];
            snprintf(commit_msg, sizeof(commit_msg), "[kaishaku] Save changes from session '%s'",
                     session);

            char commit_cmd[DEFAULT_BUFFER_SIZE + 19];
            snprintf(commit_cmd, sizeof(commit_cmd), "git commit -m \"%s\"", commit_msg);

            if (!execute_git_command(commit_cmd, NULL, 0)) {
                fprintf(stderr, "Error: Failed to save changes: %s\n", error_message);
                exit(EXIT_FAILURE);
            }
            printf("%sChanges saved successfully.%s\n", COLOR_GREEN, COLOR_RESET);
        } else if (keep) {
            // Save the current stash before exiting
            char stash_msg[DEFAULT_BUFFER_SIZE];
            snprintf(stash_msg, sizeof(stash_msg), "kaishaku: auto-stash from session '%s'",
                     session);

            char stash_cmd[DEFAULT_BUFFER_SIZE + 23];
            snprintf(stash_cmd, sizeof(stash_cmd), "git stash push -m \"%s\"", stash_msg);

            if (!execute_git_command(stash_cmd, NULL, 0)) {
                fprintf(stderr, "Warning: %s\n", error_message);
                fprintf(stderr, "Continuing without stashing changes.\n");
                // Continue despite error
            } else {
                printf(
                    "%sChanges stashed successfully. Use 'git stash list' to see your stashes.%s\n",
                    COLOR_GREEN, COLOR_RESET);
            }
        } else {
            if (!execute_git_command("git reset --hard", NULL, 0)) {
                fprintf(stderr, "Error: %s\n", error_message);
                exit(EXIT_FAILURE);
            }
            printf("%sChanges discarded.%s\n", COLOR_YELLOW, COLOR_RESET);
        }
    } else if (keep || save) {
        printf("%sNo changes to save or stash.%s\n", COLOR_YELLOW, COLOR_RESET);
    }

    // Return to original branch
    char git_cmd[DEFAULT_BUFFER_SIZE];
    snprintf(git_cmd, sizeof(git_cmd), "git checkout %s", original_branch);

    if (!execute_git_command(git_cmd, NULL, 0)) {
        fprintf(stderr, "Error: %s\n", error_message);
        exit(EXIT_FAILURE);
    }

    unlink(ACTIVE_FILE);
    printf("%sReturned to branch '%s' from session '%s'%s\n", COLOR_GREEN, original_branch, session,
           COLOR_RESET);
}

void cmd_status(void) {
    if (!file_exists(ACTIVE_FILE)) {
        printf("%sNo active kaishaku session.%s\n", COLOR_YELLOW, COLOR_RESET);
        return;
    }

    const char* session = read_from_file(ACTIVE_FILE);
    if (!session) {
        fprintf(stderr, "Error: Failed to read active session.\n");
        return;
    }

    const char* original_branch = read_from_file(SESSION_FILE(session));
    const char* head = read_from_file(HEAD_FILE(session));

    printf("%sActive session:%s %s%s\n", COLOR_CYAN, COLOR_RESET, COLOR_WHITE, session);
    printf("  %sOriginal branch:%s %s%s\n", COLOR_CYAN, COLOR_RESET, COLOR_WHITE,
           original_branch ? original_branch : "(unknown)");
    printf("  %sSession HEAD:%s %s%s\n", COLOR_CYAN, COLOR_RESET, COLOR_WHITE,
           head ? head : "(unknown)");

    printf("\n%sCurrent HEAD:%s\n", COLOR_CYAN, COLOR_RESET);
    if (system("git log --oneline -1") > -1) {
        printf("\n%sUncommitted changes:%s\n", COLOR_CYAN, COLOR_RESET);

        if (system("git status --short") > -1) {
            printf("\n%sConfiguration:%s\n", COLOR_CYAN, COLOR_RESET);
            printf("  %sconfirm_exit:%s %s%s\n", COLOR_CYAN, COLOR_RESET, COLOR_WHITE,
                   config.confirm_exit ? "yes" : "no");
            printf("  %sauto_stash:%s %s%s\n", COLOR_CYAN, COLOR_RESET, COLOR_WHITE,
                   config.auto_stash ? "yes" : "no");
            printf("  %sauto_save:%s %s%s\n", COLOR_CYAN, COLOR_RESET, COLOR_WHITE,
                   config.auto_save ? "yes" : "no");
        }
    }
}

void cmd_list(void) {
    DIR* dir;
    struct dirent* entry;

    if (!file_exists(KAISHAKU_DIR)) {
        printf("%sNo kaishaku sessions exist.%s\n", COLOR_YELLOW, COLOR_RESET);
        return;
    }

    dir = opendir(KAISHAKU_DIR);
    if (!dir) {
        fprintf(stderr, "%sError: Failed to open sessions directory: %s%s\n", COLOR_RED,
                strerror(errno), COLOR_RESET);
        exit(EXIT_FAILURE);
    }

    // Read active session first
    char active_session[DEFAULT_BUFFER_SIZE] = "";
    if (file_exists(ACTIVE_FILE)) {
        const char* session = read_from_file(ACTIVE_FILE);
        if (session) {
            // strncpy(active_session, session, sizeof(active_session) - 1);
            snprintf(active_session, sizeof(active_session), "%s", session);

            //  active_session[sizeof(active_session) - 1] = '\0';
        }
    }

    int found = 0;
    printf("%skaishaku sessions:%s\n", COLOR_CYAN, COLOR_RESET);

    while ((entry = readdir(dir)) != NULL) {
        // Skip dotfiles and the active session marker
        if (entry->d_name[0] == '.' || strcmp(entry->d_name, ".active") == 0) {
            continue;
        }

        // Check if this is a directory and has session file
        char* session_file = SESSION_FILE(entry->d_name);
        char* head_file = HEAD_FILE(entry->d_name);

        if (!file_exists(session_file) || !file_exists(head_file)) {
            // Skip corrupted sessions but warn about them
            fprintf(stderr,
                    "%sWarning: Session '%s' appears to be corrupted. Use 'recover' to fix.%s\n",
                    COLOR_YELLOW, entry->d_name, COLOR_RESET);
            continue;
        }

        const char* original_branch = read_from_file(session_file);
        const char* head = read_from_file(head_file);

        // Verify branch and commit still exist
        char branch_check[DEFAULT_BUFFER_SIZE];
        char commit_check[DEFAULT_BUFFER_SIZE];
        int branch_exists = 1;
        int commit_exists = 1;

        if (original_branch) {
            snprintf(branch_check, sizeof(branch_check), "git rev-parse --verify %s",
                     original_branch);
            branch_exists = execute_git_command(branch_check, NULL, 0);
        }

        if (head) {
            snprintf(commit_check, sizeof(commit_check), "git rev-parse --verify %s", head);
            commit_exists = execute_git_command(commit_check, NULL, 0);
        }

        // Check if this is the active session
        int is_active = (active_session[0] != '\0' && strcmp(active_session, entry->d_name) == 0);

        // Print session info with status indicators
        printf("  %s%s%s%s\n", is_active ? COLOR_GREEN : "", is_active ? "* " : "  ", COLOR_YELLOW,
               entry->d_name);

        printf("    %sLast modified:%s %s%s\n", COLOR_CYAN, COLOR_RESET, COLOR_WHITE,
               get_session_time(entry->d_name));

        printf("    %sOriginal branch:%s %s%s%s\n", COLOR_CYAN, COLOR_RESET, COLOR_WHITE,
               original_branch ? original_branch : "(unknown)", !branch_exists ? " (missing)" : "");

        printf("    %sSession HEAD:%s %s%s%s\n", COLOR_CYAN, COLOR_RESET, COLOR_WHITE,
               head ? head : "(unknown)", !commit_exists ? " (missing)" : "");

        // Add warning for corrupted sessions
        if (!branch_exists || !commit_exists) {
            printf("    %sWarning: Session may be corrupted. Use 'recover' to fix.%s\n",
                   COLOR_YELLOW, COLOR_RESET);
        }

        found = 1;
    }

    closedir(dir);

    if (!found) {
        printf("%sNo kaishaku sessions exist.%s\n", COLOR_YELLOW, COLOR_RESET);
    }
}

void cmd_clean(const char* session) {
    if (!file_exists(KAISHAKU_DIR)) {
        printf("%sNo kaishaku sessions exist.%s\n", COLOR_YELLOW, COLOR_RESET);
        return;
    }

    // If active session exists and trying to clean it, error out
    if (file_exists(ACTIVE_FILE)) {
        const char* active_session = read_from_file(ACTIVE_FILE);
        if (active_session && (!session || strcmp(session, active_session) == 0)) {
            fprintf(stderr, "Error: Cannot clean active session '%s'. Exit the session first.\n",
                    active_session);
            exit(EXIT_FAILURE);
        }
    }

    if (session) {
        // Clean specific session
        char* session_dir = SESSION_DIR(session);
        if (!file_exists(session_dir)) {
            fprintf(stderr, "Error: Session '%s' not found.\n", session);
            exit(EXIT_FAILURE);
        }

        char* session_file = SESSION_FILE(session);
        char* head_file = HEAD_FILE(session);

        if (unlink(session_file) == -1 || unlink(head_file) == -1) {
            fprintf(stderr, "Error: Failed to remove session files: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        if (rmdir(session_dir) == -1) {
            fprintf(stderr, "Error: Failed to remove session directory: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        printf("%sSession '%s' cleaned.%s\n", COLOR_GREEN, session, COLOR_RESET);
    } else {
        // Clean all sessions
        DIR* dir;
        struct dirent* entry;

        dir = opendir(KAISHAKU_DIR);
        if (!dir) {
            fprintf(stderr, "Error: Failed to open sessions directory: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        int cleaned = 0;

        while ((entry = readdir(dir)) != NULL) {
            // Skip dotfiles and the active session marker
            if (entry->d_name[0] == '.' || strcmp(entry->d_name, ".active") == 0) {
                continue;
            }

            // Check if this is a directory and has session file
            char* session_dir = SESSION_DIR(entry->d_name);
            if (!file_exists(session_dir)) {
                continue;
            }

            // Check if it's the active session
            if (file_exists(ACTIVE_FILE)) {
                const char* active_session = read_from_file(ACTIVE_FILE);
                if (active_session && strcmp(active_session, entry->d_name) == 0) {
                    continue;
                }
            }

            char* session_file = SESSION_FILE(entry->d_name);
            char* head_file = HEAD_FILE(entry->d_name);

            unlink(session_file);
            unlink(head_file);
            rmdir(session_dir);

            cleaned++;
        }

        closedir(dir);

        printf("%s%d session(s) cleaned.%s\n", COLOR_GREEN, cleaned, COLOR_RESET);
    }
}

void cmd_config(int argc, char* argv[]) {
    if (argc < 1) {
        fprintf(stderr, "Error: Missing config command.\n");
        exit(EXIT_FAILURE);
    }

    const char* cmd = argv[0];

    if (strcmp(cmd, "get") == 0) {
        if (argc < 2) {
            fprintf(stderr, "Error: Missing config key.\n");
            exit(EXIT_FAILURE);
        }

        const char* key = argv[1];

        if (strcmp(key, "confirm.exit") == 0) {
            printf("%s%d%s\n", COLOR_WHITE, config.confirm_exit, COLOR_RESET);
        } else if (strcmp(key, "auto.stash") == 0) {
            printf("%s%d%s\n", COLOR_WHITE, config.auto_stash, COLOR_RESET);
        } else if (strcmp(key, "auto.save") == 0) {
            printf("%s%d%s\n", COLOR_WHITE, config.auto_save, COLOR_RESET);
        } else {
            fprintf(stderr, "Error: Unknown config key '%s'.\n", key);
            exit(EXIT_FAILURE);
        }
    } else if (strcmp(cmd, "set") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Missing config key or value.\n");
            exit(EXIT_FAILURE);
        }

        const char* key = argv[1];
        const char* value = argv[2];

        int bool_value = atoi(value);

        if (strcmp(key, "confirm.exit") == 0) {
            config.confirm_exit = bool_value;
            printf("%sSet confirm.exit = %d%s\n", COLOR_GREEN, bool_value, COLOR_RESET);
        } else if (strcmp(key, "auto.stash") == 0) {
            config.auto_stash = bool_value;
            printf("%sSet auto.stash = %d%s\n", COLOR_GREEN, bool_value, COLOR_RESET);
        } else if (strcmp(key, "auto.save") == 0) {
            config.auto_save = bool_value;
            printf("%sSet auto.save = %d%s\n", COLOR_GREEN, bool_value, COLOR_RESET);
        } else {
            fprintf(stderr, "Error: Unknown config key '%s'.\n", key);
            exit(EXIT_FAILURE);
        }

        // Save the config to Git config
        char git_cmd[DEFAULT_BUFFER_SIZE];
        snprintf(git_cmd, sizeof(git_cmd), "git config --local kaishaku.%s %d", key, bool_value);

        if (!execute_git_command(git_cmd, NULL, 0)) {
            fprintf(stderr, "Warning: Failed to save config: %s\n", error_message);
            // Continue despite error
        }
    } else {
        fprintf(stderr, "Error: Unknown config command '%s'.\n", cmd);
        exit(EXIT_FAILURE);
    }
}

void load_config(void) {
    char output[DEFAULT_BUFFER_SIZE];
    char git_cmd[DEFAULT_BUFFER_SIZE];

    // Try to create kaishaku section in git config if it doesn't exist
    if (!file_exists(KAISHAKU_DIR)) {
        ensure_directory_exists(KAISHAKU_DIR);
    }

    // Load confirm_exit
    snprintf(git_cmd, sizeof(git_cmd), "git config --get kaishaku.confirm.exit");
    if (execute_git_command(git_cmd, output, sizeof(output))) {
        config.confirm_exit = atoi(output);
    } else {
        // Set default value if not configured
        snprintf(git_cmd, sizeof(git_cmd), "git config --local kaishaku.confirm.exit %d",
                 config.confirm_exit);
        execute_git_command(git_cmd, NULL, 0);
    }

    // Load auto_stash
    snprintf(git_cmd, sizeof(git_cmd), "git config --get kaishaku.auto.stash");
    if (execute_git_command(git_cmd, output, sizeof(output))) {
        config.auto_stash = atoi(output);
    } else {
        // Set default value if not configured
        snprintf(git_cmd, sizeof(git_cmd), "git config --local kaishaku.auto.stash %d",
                 config.auto_stash);
        execute_git_command(git_cmd, NULL, 0);
    }

    // Load auto_save
    snprintf(git_cmd, sizeof(git_cmd), "git config --get kaishaku.auto.save");
    if (execute_git_command(git_cmd, output, sizeof(output))) {
        config.auto_save = atoi(output);
    } else {
        // Set default value if not configured
        snprintf(git_cmd, sizeof(git_cmd), "git config --local kaishaku.auto.save %d",
                 config.auto_save);
        execute_git_command(git_cmd, NULL, 0);
    }
}

void cmd_recover(const char* session) {
    if (!session)
        usage();

    if (!file_exists(KAISHAKU_DIR)) {
        fprintf(stderr, "%sError: No kaishaku sessions exist.%s\n", COLOR_RED, COLOR_RESET);
        exit(EXIT_FAILURE);
    }

    char* session_dir = SESSION_DIR(session);
    if (!file_exists(session_dir)) {
        fprintf(stderr, "%sError: Session '%s' not found.%s\n", COLOR_RED, session, COLOR_RESET);
        exit(EXIT_FAILURE);
    }

    // Check if session is already active
    if (file_exists(ACTIVE_FILE)) {
        const char* active_session = read_from_file(ACTIVE_FILE);
        if (active_session && strcmp(active_session, session) == 0) {
            fprintf(stderr, "%sError: Session '%s' is already active.%s\n", COLOR_RED, session,
                    COLOR_RESET);
            exit(EXIT_FAILURE);
        }
    }

    // Verify session files
    char* session_file = SESSION_FILE(session);
    char* head_file = HEAD_FILE(session);

    if (!file_exists(session_file) || !file_exists(head_file)) {
        fprintf(stderr, "%sError: Session '%s' is corrupted. Missing required files.%s\n",
                COLOR_RED, session, COLOR_RESET);
        exit(EXIT_FAILURE);
    }

    const char* original_branch = read_from_file(session_file);
    const char* head = read_from_file(head_file);

    // Verify original branch exists
    char branch_check[DEFAULT_BUFFER_SIZE];
    snprintf(branch_check, sizeof(branch_check), "git rev-parse --verify %s", original_branch);
    if (!execute_git_command(branch_check, NULL, 0)) {
        fprintf(stderr, "%sWarning: Original branch '%s' not found. Creating new branch.%s\n",
                COLOR_YELLOW, original_branch, COLOR_RESET);

        // Create new branch from current HEAD
        char create_branch[DEFAULT_BUFFER_SIZE];
        snprintf(create_branch, sizeof(create_branch), "git checkout -b %s", original_branch);
        if (!execute_git_command(create_branch, NULL, 0)) {
            fprintf(stderr, "%sError: Failed to create branch '%s'.%s\n", COLOR_RED,
                    original_branch, COLOR_RESET);
            exit(EXIT_FAILURE);
        }
    }

    // Switch to the session
    if (!write_to_file(ACTIVE_FILE, session)) {
        fprintf(stderr, "%sError: Failed to activate session.%s\n", COLOR_RED, COLOR_RESET);
        exit(EXIT_FAILURE);
    }

    char git_cmd[DEFAULT_BUFFER_SIZE];
    snprintf(git_cmd, sizeof(git_cmd), "git checkout %s --detach", head);

    if (!execute_git_command(git_cmd, NULL, 0)) {
        fprintf(stderr, "%sError: Failed to checkout commit: %s%s\n", COLOR_RED, error_message,
                COLOR_RESET);
        exit(EXIT_FAILURE);
    }

    printf("%sRecovered session '%s'%s\n", COLOR_GREEN, session, COLOR_RESET);
}

void cmd_rename(const char* old_name, const char* new_name) {
    if (!old_name || !new_name)
        usage();

    if (!file_exists(KAISHAKU_DIR)) {
        fprintf(stderr, "%sError: No kaishaku sessions exist.%s\n", COLOR_RED, COLOR_RESET);
        exit(EXIT_FAILURE);
    }

    char* old_dir = SESSION_DIR(old_name);
    char* new_dir = SESSION_DIR(new_name);

    if (!file_exists(old_dir)) {
        fprintf(stderr, "%sError: Session '%s' not found.%s\n", COLOR_RED, old_name, COLOR_RESET);
        exit(EXIT_FAILURE);
    }

    if (file_exists(new_dir)) {
        fprintf(stderr, "%sError: Session '%s' already exists.%s\n", COLOR_RED, new_name,
                COLOR_RESET);
        exit(EXIT_FAILURE);
    }

    // Check if old session is active
    if (file_exists(ACTIVE_FILE)) {
        const char* active_session = read_from_file(ACTIVE_FILE);
        if (active_session && strcmp(active_session, old_name) == 0) {
            fprintf(stderr, "%sError: Cannot rename active session. Exit the session first.%s\n",
                    COLOR_RED, COLOR_RESET);
            exit(EXIT_FAILURE);
        }
    }

    // Rename the directory
    if (rename(old_dir, new_dir) == -1) {
        fprintf(stderr, "%sError: Failed to rename session: %s%s\n", COLOR_RED, strerror(errno),
                COLOR_RESET);
        exit(EXIT_FAILURE);
    }

    printf("%sRenamed session '%s' to '%s'%s\n", COLOR_GREEN, old_name, new_name, COLOR_RESET);
}

void cmd_abort(const char* session) {
    if (!file_exists(KAISHAKU_DIR)) {
        fprintf(stderr, "%sError: No kaishaku sessions exist.%s\n", COLOR_RED, COLOR_RESET);
        exit(EXIT_FAILURE);
    }

    // If no session specified, abort current session
    if (!session) {
        if (!file_exists(ACTIVE_FILE)) {
            fprintf(stderr, "%sError: No active session to abort.%s\n", COLOR_RED, COLOR_RESET);
            exit(EXIT_FAILURE);
        }
        session = read_from_file(ACTIVE_FILE);
        if (!session) {
            fprintf(stderr, "%sError: Failed to read active session.%s\n", COLOR_RED, COLOR_RESET);
            exit(EXIT_FAILURE);
        }
    }

    char* session_dir = SESSION_DIR(session);
    if (!file_exists(session_dir)) {
        fprintf(stderr, "%sError: Session '%s' not found.%s\n", COLOR_RED, session, COLOR_RESET);
        exit(EXIT_FAILURE);
    }

    // If this is the active session, return to original branch
    if (file_exists(ACTIVE_FILE)) {
        const char* active_session = read_from_file(ACTIVE_FILE);
        if (active_session && strcmp(active_session, session) == 0) {
            const char* original_branch = read_from_file(SESSION_FILE(session));
            if (original_branch) {
                char git_cmd[DEFAULT_BUFFER_SIZE];
                snprintf(git_cmd, sizeof(git_cmd), "git checkout %s", original_branch);
                if (!execute_git_command(git_cmd, NULL, 0)) {
                    fprintf(stderr, "%sError: Failed to return to original branch: %s%s\n",
                            COLOR_RED, error_message, COLOR_RESET);
                    exit(EXIT_FAILURE);
                }
                unlink(ACTIVE_FILE);
            }
        }
    }

    // Clean up session files
    char* session_file = SESSION_FILE(session);
    char* head_file = HEAD_FILE(session);
    char* time_file = SESSION_TIME_FILE(session);
    char* desc_file = SESSION_DESC_FILE(session);

    unlink(session_file);
    unlink(head_file);
    unlink(time_file);
    unlink(desc_file);
    rmdir(session_dir);

    printf("%sAborted session '%s'%s\n", COLOR_GREEN, session, COLOR_RESET);
}

void update_timestamp(const char* session) {
    time_t now = time(NULL);
    char timestamp[DEFAULT_BUFFER_SIZE];
    snprintf(timestamp, sizeof(timestamp), "%ld", (long)now);
    write_to_file(SESSION_TIME_FILE(session), timestamp);
}

char* get_session_time(const char* session) {
    static char time_str[DEFAULT_BUFFER_SIZE];
    const char* timestamp = read_from_file(SESSION_TIME_FILE(session));

    if (!timestamp) {
        return "unknown";
    }

    time_t t = (time_t)atol(timestamp);
    struct tm* tm_info = localtime(&t);

    if (!tm_info) {
        return "invalid";
    }

    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    return time_str;
}


int main(int argc, char* argv[]) {
    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        usage();
        return 0;
    }

    int offset = get_command_offset(argv[1]);
    if (offset == -1) {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }

    load_config();

    const char* argv2 = (argc >= 3) ? argv[2] : NULL;
    const char* argv3 = (argc >= 4) ? argv[3] : NULL;

// Yeah, this trips -Wpedantic, but this macro-generated switch is under control. Let it slide.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    switch (offset) { COMMAND_LIST(CMD_CASE) }
#pragma GCC diagnostic pop

    return 0;
}