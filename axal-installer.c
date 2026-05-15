/*
 * AXAL Kernel Installer — Single binary tool
 * 
 * Reads AXAL kernel files directly from the AXAL source tree,
 * backs up the target project's original files, then overwrites them.
 *
 * Usage (from the AXAL project root):
 *   ./axal-installer install <target_boredos_path>
 *   ./axal-installer rollback <target_boredos_path>
 *   ./axal-installer status <target_boredos_path>
 *
 * The tool finds AXAL source files relative to its own location.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>

#define BACKUP_DIR ".axal-backup"
#define MARKER_FILE BACKUP_DIR "/.axal-installed"

/* Colors */
#define C_RED     "\033[1;31m"
#define C_GREEN   "\033[1;32m"
#define C_YELLOW  "\033[1;33m"
#define C_BLUE    "\033[1;34m"
#define C_CYAN    "\033[1;36m"
#define C_RESET   "\033[0m"

/* Files that AXAL modifies (relative to project root) */
static const char *modified_files[] = {
    "src/core/version.c",
    "src/core/main.c",
    "src/mem/memory_manager.c",
    "src/mem/paging.c",
    "src/arch/syscalls.asm",
    "src/arch/interrupts.asm",
    "src/sys/idt.c",
    "src/sys/lapic.h",
    "src/sys/lapic.c",
    "src/sys/process.h",
    "src/sys/process.c",
    "Makefile",
    "linker.ld",
};
static const int modified_count = 13;

/* Files that AXAL adds (don't exist in original) */
static const char *new_files[] = {
    "src/mem/pmm.h",
    "src/mem/pmm.c",
    "src/mem/pcpu_cache.h",
    "src/mem/pcpu_cache.c",
    "src/dev/bcache.h",
    "src/dev/bcache.c",
    "src/net/tcp_socket.h",
    "src/net/tcp_socket.c",
};
static const int new_count = 8;

static char axal_root[PATH_MAX];  /* Path to AXAL source tree */

static void print_banner(void) {
    printf(C_CYAN "\n");
    printf("  ╔══════════════════════════════════════════════╗\n");
    printf("  ║         AXAL Kernel Installer v1.0          ║\n");
    printf("  ║       boredos-axal  4.2.1-1-axal            ║\n");
    printf("  ╚══════════════════════════════════════════════╝\n");
    printf(C_RESET "\n");
}

/* Create parent directories for a file path */
static void mkdirs(const char *filepath) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", filepath);
    char *last_slash = strrchr(tmp, '/');
    if (!last_slash) return;
    *last_slash = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* Copy a file from src to dst. Returns 0 on success. */
static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;

    mkdirs(dst);
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(in);
    fclose(out);
    return 0;
}

static int file_exists(const char *path) {
    return access(path, F_OK) == 0;
}

/* Resolve the AXAL source root from the binary's own location */
static int resolve_axal_root(const char *argv0) {
    char self[PATH_MAX];

    /* Try /proc/self/exe first (Linux) */
    ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (len > 0) {
        self[len] = '\0';
    } else {
        /* Fallback: use argv[0] */
        if (!realpath(argv0, self)) {
            fprintf(stderr, C_RED "[✗]" C_RESET " Cannot resolve binary path\n");
            return -1;
        }
    }

    /* Binary is at <axal_root>/axal-installer or <axal_root>/tools/axal-installer */
    char *dir = dirname(self);
    
    /* Check if we're in tools/ subdirectory */
    char check[PATH_MAX];
    snprintf(check, sizeof(check), "%s/src/core/version.c", dir);
    if (file_exists(check)) {
        strncpy(axal_root, dir, PATH_MAX - 1);
        return 0;
    }

    /* Try parent directory (binary is in tools/) */
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s/..", dir);
    if (!realpath(parent, axal_root)) {
        fprintf(stderr, C_RED "[✗]" C_RESET " Cannot resolve AXAL root\n");
        return -1;
    }

    snprintf(check, sizeof(check), "%s/src/core/version.c", axal_root);
    if (!file_exists(check)) {
        fprintf(stderr, C_RED "[✗]" C_RESET " Cannot find AXAL source tree\n");
        fprintf(stderr, "    Expected at: %s\n", axal_root);
        return -1;
    }

    return 0;
}

/* Verify target is a BoredOS project */
static int verify_target(const char *target) {
    char check[PATH_MAX];
    snprintf(check, sizeof(check), "%s/src/core/version.c", target);
    if (!file_exists(check)) {
        fprintf(stderr, C_RED "[✗]" C_RESET " Target doesn't look like a BoredOS project: %s\n", target);
        return 0;
    }
    return 1;
}

/* ============================================================ */
/* INSTALL                                                       */
/* ============================================================ */
static int do_install(const char *target) {
    print_banner();
    if (!verify_target(target)) return 1;

    char marker[PATH_MAX];
    snprintf(marker, sizeof(marker), "%s/" MARKER_FILE, target);

    if (file_exists(marker)) {
        printf(C_YELLOW "[!]" C_RESET " AXAL is already installed in this project.\n");
        printf("    Run 'rollback' first to reinstall.\n");
        return 1;
    }

    printf(C_YELLOW "  AXAL source:" C_RESET " %s\n", axal_root);
    printf(C_YELLOW "  Target:     " C_RESET " %s\n\n", target);

    /* Step 1: Backup original files from target */
    printf(C_BLUE "[*]" C_RESET " Backing up original kernel files...\n");

    char backup_dir[PATH_MAX];
    snprintf(backup_dir, sizeof(backup_dir), "%s/" BACKUP_DIR, target);
    mkdir(backup_dir, 0755);

    for (int i = 0; i < modified_count; i++) {
        char src[PATH_MAX], dst[PATH_MAX];
        snprintf(src, sizeof(src), "%s/%s", target, modified_files[i]);
        snprintf(dst, sizeof(dst), "%s/" BACKUP_DIR "/%s", target, modified_files[i]);

        if (file_exists(src)) {
            if (copy_file(src, dst) == 0) {
                printf("  " C_GREEN "↑" C_RESET " %s\n", modified_files[i]);
            } else {
                printf("  " C_RED "!" C_RESET " Failed to backup: %s\n", modified_files[i]);
            }
        }
    }
    printf(C_GREEN "[✓]" C_RESET " Backup saved\n\n");

    /* Step 2: Copy AXAL modified files from AXAL source to target */
    printf(C_BLUE "[*]" C_RESET " Installing AXAL kernel modifications...\n");

    for (int i = 0; i < modified_count; i++) {
        char src[PATH_MAX], dst[PATH_MAX];
        snprintf(src, sizeof(src), "%s/%s", axal_root, modified_files[i]);
        snprintf(dst, sizeof(dst), "%s/%s", target, modified_files[i]);

        if (file_exists(src)) {
            if (copy_file(src, dst) == 0) {
                printf("  " C_GREEN "→" C_RESET " %s\n", modified_files[i]);
            } else {
                printf("  " C_RED "!" C_RESET " Failed: %s\n", modified_files[i]);
            }
        } else {
            printf("  " C_YELLOW "?" C_RESET " Not found in AXAL: %s\n", modified_files[i]);
        }
    }

    /* Step 3: Copy AXAL new files */
    printf(C_BLUE "[*]" C_RESET " Installing new AXAL components...\n");

    for (int i = 0; i < new_count; i++) {
        char src[PATH_MAX], dst[PATH_MAX];
        snprintf(src, sizeof(src), "%s/%s", axal_root, new_files[i]);
        snprintf(dst, sizeof(dst), "%s/%s", target, new_files[i]);

        if (file_exists(src)) {
            if (copy_file(src, dst) == 0) {
                printf("  " C_GREEN "+" C_RESET " %s\n", new_files[i]);
            } else {
                printf("  " C_RED "!" C_RESET " Failed: %s\n", new_files[i]);
            }
        } else {
            printf("  " C_YELLOW "?" C_RESET " Not found in AXAL: %s\n", new_files[i]);
        }
    }

    /* Write marker */
    mkdirs(marker);
    FILE *f = fopen(marker, "w");
    if (f) { fprintf(f, "axal-4.2.1-1\n"); fclose(f); }

    printf("\n" C_GREEN "[✓]" C_RESET " AXAL kernel installed!\n\n");
    printf(C_CYAN "  Kernel:" C_RESET "  boredos-axal\n");
    printf(C_CYAN "  Version:" C_RESET " 4.2.1-1-axal\n\n");
    printf("  Build:    " C_YELLOW "cd %s && make clean && make" C_RESET "\n", target);
    printf("  Rollback: " C_YELLOW "./axal-installer rollback %s" C_RESET "\n\n", target);

    return 0;
}

/* ============================================================ */
/* ROLLBACK                                                      */
/* ============================================================ */
static int do_rollback(const char *target) {
    print_banner();
    if (!verify_target(target)) return 1;

    char marker[PATH_MAX];
    snprintf(marker, sizeof(marker), "%s/" MARKER_FILE, target);

    if (!file_exists(marker)) {
        printf(C_YELLOW "[!]" C_RESET " AXAL is not installed in this project.\n");
        return 1;
    }

    printf(C_YELLOW "  Target:" C_RESET " %s\n\n", target);

    /* Step 1: Restore backed-up files */
    printf(C_BLUE "[*]" C_RESET " Restoring original kernel files...\n");

    for (int i = 0; i < modified_count; i++) {
        char backup[PATH_MAX], dst[PATH_MAX];
        snprintf(backup, sizeof(backup), "%s/" BACKUP_DIR "/%s", target, modified_files[i]);
        snprintf(dst, sizeof(dst), "%s/%s", target, modified_files[i]);

        if (file_exists(backup)) {
            if (copy_file(backup, dst) == 0) {
                printf("  " C_GREEN "←" C_RESET " %s\n", modified_files[i]);
            }
        }
    }

    /* Step 2: Remove AXAL-added files */
    printf(C_BLUE "[*]" C_RESET " Removing AXAL-specific files...\n");

    for (int i = 0; i < new_count; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", target, new_files[i]);
        if (file_exists(path)) {
            unlink(path);
            printf("  " C_RED "✗" C_RESET " %s\n", new_files[i]);
        }
    }

    /* Step 3: Clean backup */
    printf(C_BLUE "[*]" C_RESET " Cleaning up backup...\n");

    for (int i = 0; i < modified_count; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/" BACKUP_DIR "/%s", target, modified_files[i]);
        unlink(path);
    }
    unlink(marker);

    /* Remove backup dirs (best effort) */
    const char *subdirs[] = { "src/arch", "src/core", "src/mem", "src/sys", "src/dev", "src", "" };
    for (int i = 0; subdirs[i][0] || i == 6; i++) {
        char path[PATH_MAX];
        if (subdirs[i][0])
            snprintf(path, sizeof(path), "%s/" BACKUP_DIR "/%s", target, subdirs[i]);
        else
            snprintf(path, sizeof(path), "%s/" BACKUP_DIR, target);
        rmdir(path);
        if (i == 6) break;
    }

    printf("\n" C_GREEN "[✓]" C_RESET " Rollback complete! Original kernel restored.\n\n");
    printf(C_CYAN "  Kernel:" C_RESET "  Boredkernel\n");
    printf(C_CYAN "  Version:" C_RESET " 4.2.1-dev\n\n");
    printf("  Rebuild: " C_YELLOW "cd %s && make clean && make" C_RESET "\n\n", target);

    return 0;
}

/* ============================================================ */
/* STATUS                                                        */
/* ============================================================ */
static int do_status(const char *target) {
    print_banner();
    if (!verify_target(target)) return 1;

    char marker[PATH_MAX];
    snprintf(marker, sizeof(marker), "%s/" MARKER_FILE, target);

    printf(C_YELLOW "  Target:" C_RESET " %s\n", target);
    printf(C_YELLOW "  AXAL:  " C_RESET " %s\n\n", axal_root);

    if (file_exists(marker)) {
        printf(C_GREEN "  Status: AXAL kernel INSTALLED" C_RESET "\n\n");
        printf("  Kernel:  boredos-axal\n");
        printf("  Version: 4.2.1-1-axal\n\n");
        printf("  Rollback: " C_YELLOW "./axal-installer rollback %s" C_RESET "\n\n", target);
    } else {
        printf(C_YELLOW "  Status: Stock kernel (AXAL not installed)" C_RESET "\n\n");
        printf("  Install: " C_YELLOW "./axal-installer install %s" C_RESET "\n\n", target);
    }

    return 0;
}

/* ============================================================ */
/* MAIN                                                          */
/* ============================================================ */
static void usage(const char *prog) {
    print_banner();
    printf("Usage: %s <command> <target_boredos_path>\n\n", prog);
    printf("Commands:\n");
    printf("  install <path>    Backup original kernel, install AXAL\n");
    printf("  rollback <path>   Restore original kernel from backup\n");
    printf("  status <path>     Show installation state\n");
    printf("  help              Show this message\n\n");
    printf("The tool reads AXAL files from its own source tree (auto-detected).\n");
    printf("<path> is the target BoredOS project to patch.\n\n");
    printf("Example:\n");
    printf("  ./axal-installer install ../BoredOS\n");
    printf("  ./axal-installer rollback ../BoredOS\n\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argv[0]);
        return 0;
    }

    if (argc < 3) {
        fprintf(stderr, C_RED "[✗]" C_RESET " Missing target path.\n\n");
        usage(argv[0]);
        return 1;
    }

    /* Resolve AXAL source root from binary location */
    if (resolve_axal_root(argv[0]) != 0) return 1;

    /* Resolve target path */
    char target[PATH_MAX];
    if (!realpath(argv[2], target)) {
        fprintf(stderr, C_RED "[✗]" C_RESET " Target path not found: %s\n", argv[2]);
        return 1;
    }

    if (strcmp(argv[1], "install") == 0 || strcmp(argv[1], "-i") == 0) {
        return do_install(target);
    } else if (strcmp(argv[1], "rollback") == 0 || strcmp(argv[1], "-r") == 0) {
        return do_rollback(target);
    } else if (strcmp(argv[1], "status") == 0 || strcmp(argv[1], "-s") == 0) {
        return do_status(target);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        usage(argv[0]);
        return 1;
    }
}
