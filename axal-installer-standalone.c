/*
 * AXAL Kernel Installer — Self-contained standalone binary
 * All kernel files are embedded. No external dependencies.
 * Just drop this in a BoredOS project root and run it.
 *
 * Usage:
 *   ./axal-installer-standalone install     — Backup + install AXAL
 *   ./axal-installer-standalone rollback    — Restore original kernel
 *   ./axal-installer-standalone status      — Check state
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#include "release_data.h"

#define BACKUP_DIR ".axal-backup"
#define MARKER_FILE BACKUP_DIR "/.axal-installed"

#define C_RED     "\033[1;31m"
#define C_GREEN   "\033[1;32m"
#define C_YELLOW  "\033[1;33m"
#define C_BLUE    "\033[1;34m"
#define C_CYAN    "\033[1;36m"
#define C_RESET   "\033[0m"

static void print_banner(void) {
    printf(C_CYAN "\n");
    printf("  ╔══════════════════════════════════════════════╗\n");
    printf("  ║      AXAL Kernel Installer (standalone)     ║\n");
    printf("  ║       boredos-axal  4.2.1-1-axal            ║\n");
    printf("  ╚══════════════════════════════════════════════╝\n");
    printf(C_RESET "\n");
}

static void mkdirs(const char *filepath) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", filepath);
    char *last_slash = strrchr(tmp, '/');
    if (!last_slash) return;
    *last_slash = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

static int write_file(const char *path, const unsigned char *data, unsigned int len) {
    mkdirs(path);
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, C_RED "[✗]" C_RESET " Write failed: %s\n", path); return -1; }
    if (len > 0) fwrite(data, 1, len, f);
    fclose(f);
    return 0;
}

static long read_file_alloc(const char *path, unsigned char **out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out = malloc(size);
    if (!*out) { fclose(f); return -1; }
    fread(*out, 1, size, f);
    fclose(f);
    return size;
}

static int file_exists(const char *path) { return access(path, F_OK) == 0; }

static int verify_project(void) {
    if (!file_exists("src/core/version.c") || !file_exists("Makefile")) {
        fprintf(stderr, C_RED "[✗]" C_RESET " Not in a BoredOS project root.\n");
        fprintf(stderr, "    Place this binary in the project root and run from there.\n");
        return 0;
    }
    return 1;
}

static int do_install(void) {
    print_banner();
    if (!verify_project()) return 1;

    if (file_exists(MARKER_FILE)) {
        printf(C_YELLOW "[!]" C_RESET " AXAL already installed. Run 'rollback' first.\n");
        return 1;
    }

    /* Backup */
    printf(C_BLUE "[*]" C_RESET " Backing up original kernel files...\n");
    mkdir(BACKUP_DIR, 0755);

    for (int i = 0; i < modified_count; i++) {
        if (file_exists(modified_paths[i])) {
            char bp[4096];
            snprintf(bp, sizeof(bp), BACKUP_DIR "/%s", modified_paths[i]);
            unsigned char *data = NULL;
            long size = read_file_alloc(modified_paths[i], &data);
            if (size >= 0) { write_file(bp, data, (unsigned int)size); free(data); }
            printf("  " C_GREEN "↑" C_RESET " %s\n", modified_paths[i]);
        }
    }
    printf(C_GREEN "[✓]" C_RESET " Backup saved\n\n");

    /* Install modified */
    printf(C_BLUE "[*]" C_RESET " Installing AXAL kernel...\n");
    for (int i = 0; i < modified_count; i++) {
        if (axal_mod_sizes[i] > 0) {
            write_file(modified_paths[i], axal_mod_data[i], axal_mod_sizes[i]);
            printf("  " C_GREEN "→" C_RESET " %s\n", modified_paths[i]);
        }
    }

    /* Install new */
    printf(C_BLUE "[*]" C_RESET " Installing new components...\n");
    for (int i = 0; i < new_count; i++) {
        if (axal_new_sizes[i] > 0) {
            write_file(new_paths[i], axal_new_data[i], axal_new_sizes[i]);
            printf("  " C_GREEN "+" C_RESET " %s\n", new_paths[i]);
        }
    }

    /* Marker */
    write_file(MARKER_FILE, (const unsigned char *)"axal-4.2.1-1\n", 13);

    printf("\n" C_GREEN "[✓]" C_RESET " Done!\n\n");
    printf(C_CYAN "  Kernel:" C_RESET "  boredos-axal\n");
    printf(C_CYAN "  Version:" C_RESET " 4.2.1-1-axal\n\n");
    printf("  Build:    " C_YELLOW "make clean && make" C_RESET "\n");
    printf("  Rollback: " C_YELLOW "./axal-installer-standalone rollback" C_RESET "\n\n");
    return 0;
}

static int do_rollback(void) {
    print_banner();
    if (!verify_project()) return 1;

    if (!file_exists(MARKER_FILE)) {
        printf(C_YELLOW "[!]" C_RESET " AXAL is not installed.\n");
        return 1;
    }

    printf(C_BLUE "[*]" C_RESET " Restoring original kernel...\n");
    for (int i = 0; i < modified_count; i++) {
        char bp[4096];
        snprintf(bp, sizeof(bp), BACKUP_DIR "/%s", modified_paths[i]);
        if (file_exists(bp)) {
            unsigned char *data = NULL;
            long size = read_file_alloc(bp, &data);
            if (size >= 0) { write_file(modified_paths[i], data, (unsigned int)size); free(data); }
            printf("  " C_GREEN "←" C_RESET " %s\n", modified_paths[i]);
        }
    }

    printf(C_BLUE "[*]" C_RESET " Removing AXAL files...\n");
    for (int i = 0; i < new_count; i++) {
        if (file_exists(new_paths[i])) {
            unlink(new_paths[i]);
            printf("  " C_RED "✗" C_RESET " %s\n", new_paths[i]);
        }
    }

    /* Cleanup backup */
    for (int i = 0; i < modified_count; i++) {
        char bp[4096]; snprintf(bp, sizeof(bp), BACKUP_DIR "/%s", modified_paths[i]); unlink(bp);
    }
    unlink(MARKER_FILE);
    const char *dirs[] = {"src/arch","src/core","src/mem","src/sys","src/dev","src/net","src",""};
    for (int i = 0; dirs[i][0] || i==7; i++) {
        char p[4096];
        if (dirs[i][0]) snprintf(p,sizeof(p),BACKUP_DIR "/%s",dirs[i]);
        else snprintf(p,sizeof(p),BACKUP_DIR);
        rmdir(p); if(i==7) break;
    }

    printf("\n" C_GREEN "[✓]" C_RESET " Rollback complete!\n\n");
    printf("  Rebuild: " C_YELLOW "make clean && make" C_RESET "\n\n");
    return 0;
}

static int do_status(void) {
    print_banner();
    if (!verify_project()) return 1;
    if (file_exists(MARKER_FILE))
        printf(C_GREEN "  AXAL kernel is INSTALLED" C_RESET "\n\n");
    else
        printf(C_YELLOW "  AXAL kernel is NOT installed" C_RESET "\n\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_banner();
        printf("Usage: %s <install|rollback|status>\n\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1],"install")==0) return do_install();
    if (strcmp(argv[1],"rollback")==0) return do_rollback();
    if (strcmp(argv[1],"status")==0) return do_status();
    fprintf(stderr, "Unknown: %s\n", argv[1]);
    return 1;
}
