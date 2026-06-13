/* sceUtilitySavedata: a persistent virtual memory stick.
 *
 * Save operations write real files under memstick/PSP/SAVEDATA/<gameName><saveName>/ next to
 * the executable (override the root with SR_MEMSTICK), the same layout PPSSPP and a real PSP
 * use, so saves can be copied between this build and PPSSPP. Field offsets and result codes
 * follow PPSSPP's SceUtilitySavedataParam (Core/Dialog/SavedataParam.h); the earlier no-op
 * implementation was also missing the abortStatus word, putting msFree/idList/fileList four
 * bytes too low.
 *
 * Implemented modes: AUTOLOAD/LOAD/LISTLOAD read the data file back into dataBuf;
 * AUTOSAVE/SAVE/LISTSAVE write dataBuf plus PARAM.SFO and the icon/pic/snd blobs;
 * LIST enumerates this game's save directories; FILES lists a save's files; SIZES /
 * GETSIZE report a roomy fake memory stick; the DELETE family removes a save directory.
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include "recomp.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* SceUtilitySavedataParam offsets */
enum {
    SDP_result    = 0x1c,
    SDP_mode      = 0x30,
    SDP_gameName  = 0x3c,   /* char[13] */
    SDP_saveName  = 0x4c,   /* char[20] */
    SDP_saveNameList = 0x60,
    SDP_fileName  = 0x64,   /* char[13] */
    SDP_dataBuf   = 0x74,
    SDP_dataBufSize = 0x78,
    SDP_dataSize  = 0x7c,
    SDP_sfoTitle  = 0x80,   /* char[0x80] */
    SDP_sfoSaveTitle = 0x100, /* char[0x80] */
    SDP_sfoDetail = 0x180,  /* char[0x400] */
    SDP_sfoParental = 0x580,
    SDP_icon0     = 0x584,  /* PspUtilitySavedataFileData: buf,bufSize,size,unk */
    SDP_icon1     = 0x594,
    SDP_pic1      = 0x5a4,
    SDP_snd0      = 0x5b4,
    SDP_msFree    = 0x5d0,
    SDP_msData    = 0x5d4,
    SDP_usedData  = 0x5d8,
    SDP_idList    = 0x5f4,
    SDP_fileList  = 0x5f8,
    SDP_sizeInfo  = 0x5fc,
};

/* SceUtilitySavedataType (PPSSPP SavedataParam.h). The MAKEDATA/READDATA/WRITEDATA/ERASE
 * family (13-21) is the no-dialog "secure" API many games use for the actual save IO after
 * presenting their own UI; PPSSPP routes them to the same Save/Load/Delete actions. */
enum { SD_AUTOLOAD=0, SD_AUTOSAVE=1, SD_LOAD=2, SD_SAVE=3, SD_LISTLOAD=4, SD_LISTSAVE=5,
       SD_LISTDELETE=6, SD_LISTALLDELETE=7, SD_SIZES=8, SD_AUTODELETE=9, SD_DELETE=10,
       SD_LIST=11, SD_FILES=12, SD_MAKEDATASECURE=13, SD_MAKEDATA=14, SD_READDATASECURE=15,
       SD_READDATA=16, SD_WRITEDATASECURE=17, SD_WRITEDATA=18, SD_ERASESECURE=19,
       SD_ERASE=20, SD_DELETEDATA=21, SD_GETSIZE=22 };

#define ERR_LOAD_NO_DATA    0x80110307u
#define ERR_LOAD_NOT_FOUND  0x80110309u
#define ERR_DELETE_NO_DATA  0x80110347u
#define ERR_RW_NO_DATA      0x80110327u
#define ERR_RW_NOT_FOUND    0x80110329u
#define ERR_RW_MS_FULL      0x80110323u
#define ERR_SIZES_NO_DATA   0x801103C7u   /* SIZES on a save that doesn't exist yet (PPSSPP) */

/* SceUtilitySavedataParam: bind result (offset 0x34, after mode). PPSSPP sets 1021 on every
 * successful load -- "PSP always responds this and this unlocks some games". */
#define SDP_bind 0x34

#define CLUSTER 0x8000u          /* 32 KB "memory stick" cluster */
#define FREE_CLUSTERS 0x4000u    /* pretend 512 MB free */

static void rd_cstr(uint32_t addr, char *out, int max) {
    int i = 0;
    if (addr) for (; i < max - 1; i++) {
        uint8_t c = MEM_R8(addr + (uint32_t)i);
        if (!c) break;
        out[i] = (char)c;
    }
    out[i] = 0;
}

static const char *ms_root(void) {
    const char *r = getenv("SR_MEMSTICK");
    return r && *r ? r : "memstick";
}

static void mkdirs(const char *path) {
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') { *p = 0; CreateDirectoryA(tmp, NULL); *p = '\\'; }
    }
    CreateDirectoryA(tmp, NULL);
}

/* memstick/PSP/SAVEDATA/<gameName><saveName> */
static void save_dir(char *out, int cap, const char *game, const char *save) {
    snprintf(out, cap, "%s\\PSP\\SAVEDATA\\%s%s", ms_root(), game, save);
}

static int sdlog(void) { static int v = -1; if (v < 0) v = getenv("SR_DLGLOG") ? 1 : 0; return v; }

static int write_file(const char *dir, const char *name, const uint8_t *data, uint32_t n) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = !n || fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
}

/* Write a guest FileData blob (icon0/icon1/pic1/snd0) if present. */
static void write_filedata(const char *dir, const char *name, uint32_t fd) {
    uint32_t buf = MEM_R32(fd + 0), size = MEM_R32(fd + 8);
    if (buf && size && size < 0x400000)
        write_file(dir, name, (const uint8_t *)SR_HOST(buf), size);
}

/* ---- minimal PARAM.SFO writer (PSF v1.1) -------------------------------------------------- */
typedef struct { const char *key; uint16_t fmt; uint32_t len, maxlen; const void *val; } SfoEnt;

static void sfo_write(const char *dir, const char *title, const char *saveTitle,
                      const char *detail, const char *saveDir, uint32_t parental,
                      const char *fileName) {
    static uint8_t params[128], filelist[3168];   /* zeroed secure blocks */
    /* FILE_LIST entry 0: {char name[13]; u8 hash[16]; u8 pad[3]} -- records the data file
     * (hash left zero; we don't encrypt). */
    memset(filelist, 0, sizeof(filelist));
    if (fileName && fileName[0]) {
        size_t fl = strlen(fileName); if (fl > 12) fl = 12;
        memcpy(filelist, fileName, fl);
    }
    char cat[4] = "MS";
    uint32_t pl = parental;
    SfoEnt e[8];
    int n = 0;
    /* keys must be sorted alphabetically */
    e[n++] = (SfoEnt){"CATEGORY",           0x0204, 3, 4, cat};
    e[n++] = (SfoEnt){"PARENTAL_LEVEL",     0x0404, 4, 4, &pl};
    e[n++] = (SfoEnt){"SAVEDATA_DETAIL",    0x0204, (uint32_t)strlen(detail) + 1, 1024, detail};
    e[n++] = (SfoEnt){"SAVEDATA_DIRECTORY", 0x0204, (uint32_t)strlen(saveDir) + 1, 64, saveDir};
    e[n++] = (SfoEnt){"SAVEDATA_FILE_LIST", 0x0004, sizeof(filelist), sizeof(filelist), filelist};
    e[n++] = (SfoEnt){"SAVEDATA_PARAMS",    0x0004, sizeof(params), sizeof(params), params};
    e[n++] = (SfoEnt){"SAVEDATA_TITLE",     0x0204, (uint32_t)strlen(saveTitle) + 1, 128, saveTitle};
    e[n++] = (SfoEnt){"TITLE",              0x0204, (uint32_t)strlen(title) + 1, 128, title};

    uint32_t keyOff[8], keySize = 0, dataOff[8], dataSize = 0;
    for (int i = 0; i < n; i++) {
        keyOff[i] = keySize; keySize += (uint32_t)strlen(e[i].key) + 1;
        dataOff[i] = dataSize; dataSize += (e[i].maxlen + 3) & ~3u;
    }
    uint32_t keyStart = 20 + 16u * (uint32_t)n;
    uint32_t dataStart = (keyStart + keySize + 3) & ~3u;

    uint8_t *buf = (uint8_t *)calloc(1, dataStart + dataSize);
    if (!buf) return;
    memcpy(buf, "\0PSF", 4);
    *(uint32_t *)(buf + 4) = 0x0101;
    *(uint32_t *)(buf + 8) = keyStart;
    *(uint32_t *)(buf + 12) = dataStart;
    *(uint32_t *)(buf + 16) = (uint32_t)n;
    for (int i = 0; i < n; i++) {
        uint8_t *ix = buf + 20 + 16 * i;
        *(uint16_t *)(ix + 0) = (uint16_t)keyOff[i];
        *(uint16_t *)(ix + 2) = e[i].fmt;
        *(uint32_t *)(ix + 4) = e[i].len;
        *(uint32_t *)(ix + 8) = e[i].maxlen;
        *(uint32_t *)(ix + 12) = dataOff[i];
        strcpy((char *)(buf + keyStart + keyOff[i]), e[i].key);
        memcpy(buf + dataStart + dataOff[i], e[i].val, e[i].len);
    }
    write_file(dir, "PARAM.SFO", buf, dataStart + dataSize);
    free(buf);
}

/* ---- ScePspDateTime (16 bytes) from a Windows FILETIME ------------------------------------ */
static void put_psp_time(uint32_t addr, const FILETIME *ft) {
    SYSTEMTIME st;
    memset(&st, 0, sizeof(st));
    if (ft) { FILETIME lt; FileTimeToLocalFileTime(ft, &lt); FileTimeToSystemTime(&lt, &st); }
    MEM_W16(addr + 0, st.wYear); MEM_W16(addr + 2, st.wMonth); MEM_W16(addr + 4, st.wDay);
    MEM_W16(addr + 6, st.wHour); MEM_W16(addr + 8, st.wMinute); MEM_W16(addr + 10, st.wSecond);
    MEM_W32(addr + 12, (uint32_t)st.wMilliseconds * 1000u);
}

static void wr_fixed(uint32_t addr, const char *s, int n) {
    for (int i = 0; i < n; i++) MEM_W8(addr + (uint32_t)i, i < (int)strlen(s) ? (uint8_t)s[i] : 0);
}

/* ---- minimal PARAM.SFO reader --------------------------------------------------------------
 * Load must hand the SFO strings BACK to the game (PPSSPP SavedataParam::LoadSFO): games keep
 * their profile/pilot name in SAVEDATA_TITLE and read sfoParam after a load -- without this
 * the name they saved comes back empty. */
static void wr_guest_bytes(uint32_t addr, const uint8_t *v, uint32_t len, uint32_t cap) {
    if (len > cap) len = cap;
    for (uint32_t i = 0; i < len; i++) MEM_W8(addr + i, v[i]);
    for (uint32_t i = len; i < cap; i++) MEM_W8(addr + i, 0);
}

static void load_sfo_param(uint32_t param, const char *dir) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\PARAM.SFO", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 20 || n > (1 << 20)) { fclose(f); return; }
    uint8_t *b = (uint8_t *)malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return; }
    fclose(f);
    if (memcmp(b, "\0PSF", 4) != 0) { free(b); return; }
    uint32_t keyStart, dataStart, cnt;
    memcpy(&keyStart, b + 8, 4); memcpy(&dataStart, b + 12, 4); memcpy(&cnt, b + 16, 4);
    for (uint32_t i = 0; i < cnt && 20 + 16 * (i + 1) <= (uint32_t)n; i++) {
        const uint8_t *ix = b + 20 + 16 * i;
        uint16_t keyOff, fmt; uint32_t len, dataOff;
        memcpy(&keyOff, ix + 0, 2); memcpy(&fmt, ix + 2, 2);
        memcpy(&len, ix + 4, 4); memcpy(&dataOff, ix + 12, 4);
        if (keyStart + keyOff >= (uint32_t)n || dataStart + dataOff + len > (uint32_t)n) continue;
        const char *key = (const char *)b + keyStart + keyOff;
        const uint8_t *val = b + dataStart + dataOff;
        if (fmt == 0x0204 && !strcmp(key, "TITLE"))
            wr_guest_bytes(param + SDP_sfoTitle, val, len, 128);
        else if (fmt == 0x0204 && !strcmp(key, "SAVEDATA_TITLE"))
            wr_guest_bytes(param + SDP_sfoSaveTitle, val, len, 128);
        else if (fmt == 0x0204 && !strcmp(key, "SAVEDATA_DETAIL"))
            wr_guest_bytes(param + SDP_sfoDetail, val, len, 1024);
        else if (fmt == 0x0404 && len >= 4 && !strcmp(key, "PARENTAL_LEVEL")) {
            uint32_t pl; memcpy(&pl, val, 4);
            MEM_W32(param + SDP_sfoParental, pl);
        }
    }
    free(b);
}

/* ---- modes -------------------------------------------------------------------------------- */

static uint32_t do_save(uint32_t param, const char *game, const char *save) {
    char dir[MAX_PATH], fileName[16], title[129], saveTitle[129], detail[1025], saveDir[64];
    save_dir(dir, sizeof(dir), game, save);
    mkdirs(dir);
    rd_cstr(param + SDP_fileName, fileName, sizeof(fileName));
    uint32_t dataBuf = MEM_R32(param + SDP_dataBuf);
    uint32_t dataSize = MEM_R32(param + SDP_dataSize);
    if (fileName[0] && dataBuf && dataSize && dataSize < 0x04000000) {
        if (!write_file(dir, fileName, (const uint8_t *)SR_HOST(dataBuf), dataSize))
            return 0x80110381u;   /* SAVE_NO_MS: couldn't write */
    }
    rd_cstr(param + SDP_sfoTitle, title, sizeof(title));
    rd_cstr(param + SDP_sfoSaveTitle, saveTitle, sizeof(saveTitle));
    rd_cstr(param + SDP_sfoDetail, detail, sizeof(detail));
    snprintf(saveDir, sizeof(saveDir), "%s%s", game, save);
    sfo_write(dir, title, saveTitle, detail, saveDir, MEM_R8(param + SDP_sfoParental), fileName);
    write_filedata(dir, "ICON0.PNG", param + SDP_icon0);
    write_filedata(dir, "ICON1.PMF", param + SDP_icon1);
    write_filedata(dir, "PIC1.PNG", param + SDP_pic1);
    write_filedata(dir, "SND0.AT3", param + SDP_snd0);
    if (sdlog()) fprintf(stderr, "savedata: SAVE %s\\%s (%u bytes)\n", dir, fileName, dataSize);
    return 0;
}

static int dir_exists(const char *dir) {
    DWORD a = GetFileAttributesA(dir);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Read a host file into a guest PspUtilitySavedataFileData block {buf, bufSize, size, unk}.
 * PPSSPP loads ICON0/ICON1/PIC1/SND0 back on every Load; some games require it. */
static void load_filedata(const char *dir, const char *name, uint32_t fd) {
    uint32_t buf = MEM_R32(fd + 0), cap = MEM_R32(fd + 4);
    if (!buf || !cap) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t rd = fread(SR_HOST(buf), 1, cap, f);
    fclose(f);
    MEM_W32(fd + 8, (uint32_t)rd);
}

static uint32_t do_load(uint32_t param, const char *game, const char *save) {
    char dir[MAX_PATH], fileName[16], path[MAX_PATH];
    save_dir(dir, sizeof(dir), game, save);
    rd_cstr(param + SDP_fileName, fileName, sizeof(fileName));
    if (!dir_exists(dir)) {
        if (sdlog()) fprintf(stderr, "savedata: LOAD %s -> no data\n", dir);
        return ERR_LOAD_NO_DATA;
    }
    MEM_W32(param + SDP_dataSize, 0);      /* forced to zero before loading (PPSSPP) */
    /* Blank fileName means success without reading data (PPSSPP LoadSaveData); the SFO and
     * icon blobs are still handed back. */
    if (fileName[0]) {
        snprintf(path, sizeof(path), "%s\\%s", dir, fileName);
        FILE *f = fopen(path, "rb");
        if (!f) {
            if (sdlog()) fprintf(stderr, "savedata: LOAD %s -> file not found\n", path);
            return ERR_LOAD_NOT_FOUND;
        }
        uint32_t dataBuf = MEM_R32(param + SDP_dataBuf);
        uint32_t cap = MEM_R32(param + SDP_dataBufSize);
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n < 0) n = 0;
        if (cap && (uint32_t)n > cap) n = (long)cap;
        size_t rd = dataBuf ? fread(SR_HOST(dataBuf), 1, (size_t)n, f) : 0;
        fclose(f);
        MEM_W32(param + SDP_dataSize, (uint32_t)rd);
        if (sdlog()) {
            const uint8_t *p = (const uint8_t *)SR_HOST(dataBuf);
            fprintf(stderr, "savedata: LOAD %s (%u bytes) -> buf=0x%08x cap=%u first=%02x%02x%02x%02x\n",
                    path, (unsigned)rd, dataBuf, cap, p[0], p[1], p[2], p[3]);
        }
    } else if (sdlog()) {
        fprintf(stderr, "savedata: LOAD %s (blank fileName: SFO/icons only)\n", dir);
    }
    /* copy the resolved save dir name back into the request (PPSSPP) */
    wr_fixed(param + SDP_saveName, save, 20);
    load_sfo_param(param, dir);            /* hand TITLE/SAVEDATA_TITLE/... back (LoadSFO) */
    load_filedata(dir, "ICON0.PNG", param + SDP_icon0);
    load_filedata(dir, "ICON1.PMF", param + SDP_icon1);
    load_filedata(dir, "PIC1.PNG", param + SDP_pic1);
    load_filedata(dir, "SND0.AT3", param + SDP_snd0);
    /* ACX profile-enumeration diagnostic (ULUS10176): the records-load state machine
     * (sub_3717C) accepts a USERID slot as a registered pilot only when the mode-15 secure
     * read returns result 0 AND the 16 bytes at dataBuf+4 match the live record template at
     * runtime 0x08A80DF8 (dword_27CDF4+4); otherwise the slot is judged empty and skipped, so
     * nothing lands in the profile-slot array / occupancy bitmask. Dump both so a single
     * SR_DLGLOG run shows whether that compare would pass. */
    if (sdlog()) {
        uint32_t db = MEM_R32(param + SDP_dataBuf);
        fprintf(stderr, "savedata: SECRD dir=%s dataSize=%u data+4=", dir,
                MEM_R32(param + SDP_dataSize));
        for (uint32_t i = 4; i < 20; i++) fprintf(stderr, "%02x", db ? MEM_R8(db + i) : 0);
        fprintf(stderr, " tmpl@0x08A80DF8=");
        for (uint32_t i = 0; i < 16; i++) fprintf(stderr, "%02x", MEM_R8(0x08A80DF8u + i));
        fprintf(stderr, "\n");
    }
    MEM_W32(param + SDP_bind, 1021);       /* PSP always responds this; unlocks some games */
    return 0;
}

static uint32_t do_delete(const char *game, const char *save) {
    char dir[MAX_PATH], path[MAX_PATH];
    save_dir(dir, sizeof(dir), game, save);
    WIN32_FIND_DATAA fd;
    snprintf(path, sizeof(path), "%s\\*", dir);
    HANDLE h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE) return ERR_DELETE_NO_DATA;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
            DeleteFileA(path);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    RemoveDirectoryA(dir);
    return 0;
}

/* ERASE/ERASESECURE: remove just the named data file inside the save dir (PPSSPP DeleteData). */
static uint32_t do_erase(uint32_t param, const char *game, const char *save) {
    char dir[MAX_PATH], fileName[16], path[MAX_PATH];
    save_dir(dir, sizeof(dir), game, save);
    rd_cstr(param + SDP_fileName, fileName, sizeof(fileName));
    if (!fileName[0]) return ERR_RW_NO_DATA;
    snprintf(path, sizeof(path), "%s\\%s", dir, fileName);
    if (sdlog()) fprintf(stderr, "savedata: ERASE %s\n", path);
    return DeleteFileA(path) ? 0 : ERR_RW_NO_DATA;
}

/* LIST (11): fill idList with this game's save directories. */
static uint32_t do_list(uint32_t param, const char *game) {
    uint32_t idList = MEM_R32(param + SDP_idList);
    if (!idList) return 0;
    uint32_t maxCount = MEM_R32(idList + 0);
    uint32_t entries = MEM_R32(idList + 8);
    uint32_t count = 0;
    char pat[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s\\PSP\\SAVEDATA\\%s*", ms_root(), game);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        size_t gl = strlen(game);
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == '.') continue;
            if (entries && count < maxCount) {
                uint32_t e = entries + count * 72u;       /* SceUtilitySavedataIdListEntry */
                MEM_W32(e + 0, 0x11FF);                   /* st_mode (directory) */
                put_psp_time(e + 4, &fd.ftCreationTime);
                put_psp_time(e + 20, &fd.ftLastAccessTime);
                put_psp_time(e + 36, &fd.ftLastWriteTime);
                wr_fixed(e + 52, fd.cFileName + gl, 20);  /* saveName part only */
            }
            count++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (count > maxCount) count = maxCount;
    MEM_W32(idList + 4, count);                           /* resultCount */
    if (sdlog()) fprintf(stderr, "savedata: LIST %s* -> %u saves\n", game, count);
    return 0;
}

static int is_system_file(const char *n) {
    return !_stricmp(n, "PARAM.SFO") || !_stricmp(n, "ICON0.PNG") || !_stricmp(n, "ICON1.PMF") ||
           !_stricmp(n, "PIC1.PNG") || !_stricmp(n, "SND0.AT3");
}

/* FILES (12): list the files inside one save directory. */
static uint32_t do_files(uint32_t param, const char *game, const char *save) {
    uint32_t fl = MEM_R32(param + SDP_fileList);
    if (!fl) return 0;
    uint32_t maxSec = MEM_R32(fl + 0), maxNorm = MEM_R32(fl + 4), maxSys = MEM_R32(fl + 8);
    uint32_t pSec = MEM_R32(fl + 24), pNorm = MEM_R32(fl + 28), pSys = MEM_R32(fl + 32);
    uint32_t nSec = 0, nNorm = 0, nSys = 0;
    char pat[MAX_PATH], dir[MAX_PATH];
    save_dir(dir, sizeof(dir), game, save);
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        MEM_W32(fl + 12, 0); MEM_W32(fl + 16, 0); MEM_W32(fl + 20, 0);
        return ERR_LOAD_NO_DATA;                  /* no such save: FILES_NO_DATA semantics */
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        int sys = is_system_file(fd.cFileName);
        uint32_t *cnt = sys ? &nSys : &nNorm;
        uint32_t base = sys ? pSys : pNorm, cap = sys ? maxSys : maxNorm;
        for (int pass = 0; pass < (sys ? 1 : 2); pass++) {
            if (pass == 1) { cnt = &nSec; base = pSec; cap = maxSec; }   /* data files also "secure" */
            if (base && *cnt < cap) {
                uint32_t e = base + *cnt * 80u;            /* SceUtilitySavedataFileListEntry */
                MEM_W32(e + 0, 0x21FF);                    /* st_mode (file) */
                MEM_W32(e + 4, 0);
                MEM_W32(e + 8, fd.nFileSizeLow);
                MEM_W32(e + 12, 0);
                put_psp_time(e + 16, &fd.ftCreationTime);
                put_psp_time(e + 32, &fd.ftLastAccessTime);
                put_psp_time(e + 48, &fd.ftLastWriteTime);
                wr_fixed(e + 64, fd.cFileName, 16);
            }
            (*cnt)++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (nSec > maxSec) nSec = maxSec;
    if (nNorm > maxNorm) nNorm = maxNorm;
    if (nSys > maxSys) nSys = maxSys;
    MEM_W32(fl + 12, nSec); MEM_W32(fl + 16, nNorm); MEM_W32(fl + 20, nSys);
    return 0;
}

static uint32_t do_sizes(uint32_t param, const char *game) {
    uint32_t msFree = MEM_R32(param + SDP_msFree);
    if (msFree) {
        MEM_W32(msFree + 0, CLUSTER);
        MEM_W32(msFree + 4, FREE_CLUSTERS);
        MEM_W32(msFree + 8, FREE_CLUSTERS * (CLUSTER / 0x400));
        wr_fixed(msFree + 12, "512 MB", 8);
    }
    int sizes_no_data = 0;
    uint32_t msData = MEM_R32(param + SDP_msData);
    if (msData) {
        /* info block at +36: used clusters/KB of the named save (0 if absent) */
        char game2[16], save2[24], dir[MAX_PATH], pat[MAX_PATH];
        rd_cstr(msData + 0, game2, 14);
        rd_cstr(msData + 16, save2, 21);
        save_dir(dir, sizeof(dir), game2[0] ? game2 : game, save2);
        if (dir_exists(dir)) {
            uint64_t used = 0;
            snprintf(pat, sizeof(pat), "%s\\*", dir);
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(pat, &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                        used += ((uint64_t)fd.nFileSizeLow + CLUSTER - 1) / CLUSTER * CLUSTER;
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
            MEM_W32(msData + 36, (uint32_t)(used / CLUSTER));
            MEM_W32(msData + 40, (uint32_t)(used / 0x400));
            wr_fixed(msData + 44, "", 8);
            MEM_W32(msData + 52, (uint32_t)(used / 0x400));
            wr_fixed(msData + 56, "", 8);
        } else {
            /* PPSSPP SavedataParam::GetSizes: a SIZES query for a save that does NOT exist
             * zeroes the used-space block and returns SIZES_NO_DATA — that's how the game
             * learns "this is a brand-new save". Our old code always returned 0 ("it exists"),
             * so the game treated a freshly-created pilot as an overwrite of existing data and
             * skipped registering it into a profile slot (menu stayed USER:UNKNOWN). */
            MEM_W32(msData + 36, 0);
            MEM_W32(msData + 40, 0);
            wr_fixed(msData + 44, "", 8);
            MEM_W32(msData + 52, 0);
            wr_fixed(msData + 56, "", 8);
            sizes_no_data = 1;
        }
    }
    uint32_t ud = MEM_R32(param + SDP_usedData);
    if (ud) {
        uint32_t total = CLUSTER + CLUSTER;                 /* directory record + SFO */
        if (MEM_R8(param + SDP_fileName) != 0) {
            uint32_t ds = MEM_R32(param + SDP_dataSize);
            total += ((ds + CLUSTER - 1) / CLUSTER) * CLUSTER;
        }
        uint32_t sizes[4] = { MEM_R32(param + SDP_icon0 + 8), MEM_R32(param + SDP_icon1 + 8),
                              MEM_R32(param + SDP_pic1 + 8), MEM_R32(param + SDP_snd0 + 8) };
        for (int i = 0; i < 4; i++) total += ((sizes[i] + CLUSTER - 1) / CLUSTER) * CLUSTER;
        MEM_W32(ud + 0, total / CLUSTER);
        MEM_W32(ud + 4, total / 0x400);
        wr_fixed(ud + 8, "", 8);
        MEM_W32(ud + 16, total / 0x400);
        wr_fixed(ud + 20, "", 8);
    }
    return sizes_no_data ? ERR_SIZES_NO_DATA : 0;
}

/* GETSIZE (22): free/needed space for the sizeInfo block. */
static uint32_t do_getsize(uint32_t param) {
    uint32_t si = MEM_R32(param + SDP_sizeInfo);
    if (!si) return 0;
    uint32_t nSec = MEM_R32(si + 0), nNorm = MEM_R32(si + 4);
    uint32_t pSec = MEM_R32(si + 8), pNorm = MEM_R32(si + 12);
    uint64_t needed = CLUSTER + CLUSTER;                    /* dir record + SFO */
    for (uint32_t i = 0; i < nSec && pSec; i++) {
        uint64_t sz = MEM_R32(pSec + i * 24u) | ((uint64_t)MEM_R32(pSec + i * 24u + 4) << 32);
        needed += (sz + CLUSTER - 1) / CLUSTER * CLUSTER;
    }
    for (uint32_t i = 0; i < nNorm && pNorm; i++) {
        uint64_t sz = MEM_R32(pNorm + i * 24u) | ((uint64_t)MEM_R32(pNorm + i * 24u + 4) << 32);
        needed += (sz + CLUSTER - 1) / CLUSTER * CLUSTER;
    }
    MEM_W32(si + 16, CLUSTER);                              /* sectorSize */
    MEM_W32(si + 20, FREE_CLUSTERS);                        /* freeSectors */
    MEM_W32(si + 24, FREE_CLUSTERS * (CLUSTER / 0x400));    /* freeKB */
    wr_fixed(si + 28, "512 MB", 8);
    MEM_W32(si + 36, (uint32_t)(needed / 0x400));           /* neededKB */
    wr_fixed(si + 40, "", 8);
    MEM_W32(si + 48, (uint32_t)(needed / 0x400));           /* overwriteKB */
    wr_fixed(si + 52, "", 8);
    return 0;
}

/* Resolve the effective saveName (PPSSPP GetSaveDirName): "<>" is a wildcard meaning "any
 * existing save"; the LIST modes carry a saveNameList the user would normally pick from in
 * the system UI -- headless, auto-select the first entry whose directory exists (falling
 * back to the first entry for a fresh LISTSAVE). */
static void resolve_save(uint32_t param, uint32_t mode, const char *game, char *save, int cap) {
    rd_cstr(param + SDP_saveName, save, cap);
    int wild = !strcmp(save, "<>");
    int isList = (mode == SD_LISTLOAD || mode == SD_LISTSAVE || mode == SD_LISTDELETE);
    uint32_t list = MEM_R32(param + SDP_saveNameList);
    if ((isList || wild) && list) {
        char ent[24], first[24] = "", dir[MAX_PATH];
        for (int i = 0; i < 99; i++) {
            rd_cstr(list + (uint32_t)i * 20, ent, 21);
            if (!ent[0]) break;
            if (!strcmp(ent, "<>")) continue;
            if (!first[0]) snprintf(first, sizeof(first), "%s", ent);
            save_dir(dir, sizeof(dir), game, ent);
            if (dir_exists(dir)) { snprintf(save, (size_t)cap, "%s", ent); return; }
        }
        if (mode == SD_LISTSAVE && first[0]) { snprintf(save, (size_t)cap, "%s", first); return; }
    }
    if (wild) {
        /* no list (or none existed): first existing dir matching <game>* */
        char pat[MAX_PATH];
        WIN32_FIND_DATAA fd;
        snprintf(pat, sizeof(pat), "%s\\PSP\\SAVEDATA\\%s*", ms_root(), game);
        HANDLE h = FindFirstFileA(pat, &fd);
        save[0] = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    snprintf(save, (size_t)cap, "%s", fd.cFileName + strlen(game));
                    break;
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
}

uint32_t sr_savedata_execute(uint32_t param) {
    if (!param) return 0;
    uint32_t mode = MEM_R32(param + SDP_mode);
    char game[16], save[24];
    rd_cstr(param + SDP_gameName, game, 14);
    resolve_save(param, mode, game, save, 21);
    if (sdlog()) fprintf(stderr, "savedata: mode=%u game='%s' save='%s'\n", mode, game, save);
    switch (mode) {
        case SD_AUTOSAVE: case SD_SAVE: case SD_LISTSAVE:
        case SD_MAKEDATA: case SD_MAKEDATASECURE:
        case SD_WRITEDATA: case SD_WRITEDATASECURE: {
            uint32_t r = do_save(param, game, save);
            /* PPSSPP: MAKEDATA reports a full stick with the RW error code */
            if (r == 0x80110381u && (mode == SD_MAKEDATA || mode == SD_MAKEDATASECURE))
                r = ERR_RW_MS_FULL;
            return r;
        }
        case SD_AUTOLOAD: case SD_LOAD: case SD_LISTLOAD:
            return do_load(param, game, save);
        case SD_READDATA: case SD_READDATASECURE: {
            uint32_t r = do_load(param, game, save);             /* PPSSPP error remap */
            if (r == ERR_LOAD_NO_DATA) r = ERR_RW_NO_DATA;
            if (r == ERR_LOAD_NOT_FOUND) r = ERR_RW_NOT_FOUND;
            return r;
        }
        case SD_LISTDELETE: case SD_DELETE: case SD_AUTODELETE:
            return do_delete(game, save);
        case SD_DELETEDATA: {
            uint32_t r = do_delete(game, save);
            return r == ERR_DELETE_NO_DATA ? ERR_RW_NO_DATA : r;
        }
        case SD_ERASE: case SD_ERASESECURE:
            return do_erase(param, game, save);
        case SD_LIST:
            return do_list(param, game);
        case SD_FILES:
            return do_files(param, game, save);
        case SD_SIZES:
            return do_sizes(param, game);
        case SD_GETSIZE:
            return do_getsize(param);
        default:
            if (sdlog()) fprintf(stderr, "savedata: UNHANDLED mode=%u\n", mode);
            return 0;
    }
}
