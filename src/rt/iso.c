/* ISO9660 reader for the game disc, backing the IoFileMgrForUser HLE (sceIoOpen/Read/...).
 *
 * The game opens files by path (e.g. "disc0:/PSP_GAME/USRDIR/...") and reads them into guest
 * memory. This walks the ISO9660 directory tree on the host ISO to resolve a path to its
 * (lba,size) extent, then serves bytes from there. Only the plain ISO9660 features a PSP UMD
 * uses are handled (8.3-ish names with ';1' version suffixes, directories, one extent each).
 *
 * The ISO path comes from the PSP_ISO environment variable.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "iso.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 2048

static FILE *s_iso = NULL;
static uint32_t s_iso_offset = 0;

static void detect_wrapper(void) {
    s_iso_offset = 0;
    unsigned char pvd[SECTOR];
    fseek(s_iso, 16L * SECTOR, SEEK_SET);
    if (fread(pvd, 1, SECTOR, s_iso) != SECTOR) return;
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) return;
    
    uint32_t root_lba, root_size;
    memcpy(&root_lba, pvd + 156 + 2, 4);
    memcpy(&root_size, pvd + 156 + 10, 4);
    
    uint32_t nbytes = ((root_size + SECTOR - 1) / SECTOR) * SECTOR;
    unsigned char *buf = (unsigned char *)malloc(nbytes);
    if (!buf) return;
    fseek(s_iso, (long)root_lba * SECTOR, SEEK_SET);
    if (fread(buf, 1, nbytes, s_iso) == nbytes) {
        uint32_t o = 0;
        while (o < root_size) {
            unsigned char rl = buf[o];
            if (rl == 0) { o = ((o / SECTOR) + 1) * SECTOR; continue; }
            uint32_t ext;
            memcpy(&ext, buf + o + 2, 4);
            unsigned char nl = buf[o + 32];
            const unsigned char *nm = buf + o + 33;
            if (nl == 11 && memcmp(nm, "USER_L0.IMG", 11) == 0) {
                s_iso_offset = ext;
                fprintf(stderr, "iso: detected wrapped ISO (USER_L0.IMG at sector %u)\n", ext);
                break;
            }
            o += rl;
        }
    }
    free(buf);
}

int iso_init(void) {
    if (s_iso) return 0;
    const char *path = getenv("PSP_ISO");
    if (!path) path = "game.iso";
    s_iso = fopen(path, "rb");
    if (!s_iso) { fprintf(stderr, "iso: cannot open %s\n", path); return -1; }
    detect_wrapper();
    return 0;
}

static void read_sectors(uint32_t lba, void *buf, uint32_t bytes) {
    fseek(s_iso, (long)(s_iso_offset + lba) * SECTOR, SEEK_SET);
    if (fread(buf, 1, bytes, s_iso) != bytes) memset(buf, 0, bytes);
}

/* Normalize a guest path: drop a leading device ("disc0:", "umd0:", "ms0:") and any leading
 * slashes, returning a pointer to the first path component. */
static const char *strip_device(const char *p) {
    const char *c = strchr(p, ':');
    if (c) p = c + 1;
    while (*p == '/' || *p == '\\') p++;
    return p;
}

/* Case-insensitive compare of a path component against an ISO directory entry name, ignoring a
 * trailing ";1" version on the ISO side. comp runs until '/' or end. */
static int name_eq(const char *comp, int complen, const unsigned char *isoname, int isolen) {
    int n = isolen;
    if (n >= 2 && isoname[n - 2] == ';') n -= 2;     /* strip ;1 */
    if (n != complen) return 0;
    for (int i = 0; i < n; i++) {
        char a = (char)isoname[i], b = comp[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

/* Find a child named comp[0..complen) in the directory at (lba,size). On success fills out_lba/
 * out_size/out_isdir and returns 1. */
static int dir_find(uint32_t lba, uint32_t size, const char *comp, int complen,
                    uint32_t *out_lba, uint32_t *out_size, int *out_isdir) {
    uint32_t nbytes = ((size + SECTOR - 1) / SECTOR) * SECTOR;
    unsigned char *buf = (unsigned char *)malloc(nbytes);
    read_sectors(lba, buf, nbytes);
    uint32_t o = 0;
    int found = 0;
    while (o < size) {
        unsigned char rl = buf[o];
        if (rl == 0) { o = ((o / SECTOR) + 1) * SECTOR; continue; }
        uint32_t ext, sz;
        memcpy(&ext, buf + o + 2, 4);
        memcpy(&sz, buf + o + 10, 4);
        unsigned char flags = buf[o + 25];
        unsigned char nl = buf[o + 32];
        const unsigned char *nm = buf + o + 33;
        if (nl != 1 || (nm[0] != 0 && nm[0] != 1)) {   /* skip '.' and '..' entries */
            if (name_eq(comp, complen, nm, nl)) {
                *out_lba = ext; *out_size = sz; *out_isdir = (flags & 2) ? 1 : 0;
                found = 1; break;
            }
        }
        o += rl;
    }
    free(buf);
    return found;
}

int iso_lookup(const char *guest_path, uint32_t *out_lba, uint32_t *out_size) {
    if (iso_init() != 0) return -1;

    /* Raw UMD sector access: "sce_lbn<lbn>_size<size>" reads <size> bytes from sector <lbn>.
     * Both fields may be decimal or 0x-hex. The game uses this to stream data by LBN. */
    const char *lbn = strstr(guest_path, "sce_lbn");
    if (lbn) {
        lbn += 7;
        char *e;
        unsigned long L = strtoul(lbn, &e, 0);
        const char *sz = strstr(e, "_size");
        unsigned long S = sz ? strtoul(sz + 5, NULL, 0) : SECTOR;
        *out_lba = (uint32_t)L; *out_size = (uint32_t)S;
        return 0;
    }

    unsigned char pvd[SECTOR];
    read_sectors(16, pvd, SECTOR);
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) return -1;
    uint32_t lba, size;
    memcpy(&lba, pvd + 156 + 2, 4);
    memcpy(&size, pvd + 156 + 10, 4);
    int isdir = 1;

    const char *p = strip_device(guest_path);
    while (*p) {
        const char *slash = p;
        while (*slash && *slash != '/' && *slash != '\\') slash++;
        int complen = (int)(slash - p);
        if (complen == 0) { p = slash; if (*p) p++; continue; }
        if (!isdir) return -1;                     /* path descends into a file */
        if (!dir_find(lba, size, p, complen, &lba, &size, &isdir)) return -1;
        p = slash;
        while (*p == '/' || *p == '\\') p++;
    }
    *out_lba = lba; *out_size = size;
    return 0;
}

int iso_read(uint32_t lba, uint32_t offset, void *dst, uint32_t bytes) {
    if (iso_init() != 0) return -1;
    fseek(s_iso, (long)(s_iso_offset + lba) * SECTOR + (long)offset, SEEK_SET);
    return (int)fread(dst, 1, bytes, s_iso);
}
