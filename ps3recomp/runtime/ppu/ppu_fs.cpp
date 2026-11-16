/*
 * ps3recomp - cellFs VFS (sys_fs HLE)
 *
 * Backs the game's file I/O with the real host filesystem so it can load its
 * data/config/assets. Guest PS3 paths (/dev_bdvd/..., /app_home/..., /dev_hdd0,
 * etc.) are translated to a host root (the game directory that contains
 * PS3_GAME), set by the boot harness from the EBOOT path (or $PS3_VFS_ROOT).
 *
 * Only the calls the boot actually imports are implemented:
 *   cellFsOpen/Close/Read/Write/Lseek/Stat/Fstat/Opendir/Readdir/Closedir/
 *   Mkdir/Rmdir/Unlink/Fsync.
 *
 * Context-aware HLE: guest pointers (path, buffers, out params) are read/written
 * through vm_base in big-endian.
 */
#include "ppu_recomp.h"      /* ppu_context */
#include "../../libs/filesystem/edat.h"
#include "ps3emu/nid.h"      /* ps3_compute_nid */
#include "ps3emu/guest_call.h" /* ps3_invoke_guest: AIO completion is a guest OPD */
#include "sdata_decrypt.h"   /* SDATA/EDAT (NPD) decryption for cellFsSdataOpen */
#include "ps3emu/hz_stats.h"   /* v225A hitch attribution: guest file-read time */
#include "../platform/host_file.h"
#include "../platform/win32_compat.h"
#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <memory>
#include <mutex>
#include <thread>   /* v114 */
#include <chrono>
#include "../platform/win32_dirent.h"   /* <dirent.h>, or a Win32 stand-in */

/* Guest-resolving host backtrace (ppu_loader.cpp). Must be declared at file scope:
 * `extern "C"` inside a function body is ill-formed in C++. */
extern "C" void ydkj_host_bt(const char* tag);
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>          /* open/close on MinGW */
#else
#include <unistd.h>
#endif
#ifndef O_BINARY
#define O_BINARY 0       /* POSIX has no text/binary distinction */
#endif

extern "C" uint8_t* vm_base;
extern "C" void ppu_guest_caller(char* out, size_t n);
extern "C" uint32_t ppu_vm_size;
extern "C" void     ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
extern "C" void     vm_write32(uint64_t a, uint32_t v);
extern "C" void     vm_write64(uint64_t a, uint64_t v);
#include "../platform/win32_backtrace.h"   /* RtlCaptureStackBackTrace / GetModuleHandleA on POSIX */

/* Host root that the PS3 mount points map into (dir containing PS3_GAME). */
extern "C" const char* ppu_vfs_root = ".";

/* CELL_FS return / mode / flag constants. */
#define CELL_OK              0
#define CELL_FS_ENOENT       (-2147418106)   /* 0x80010006 -- was -2147418090, which is 0x80010016 = CELL_ENOTCONN, not ENOENT */
#define CELL_FS_EISDIR       (-2147418094)   /* 0x80010012 */
#define CELL_FS_EIO          (-2147418111)
#define CELL_FS_S_IFDIR      0x4000u
#define CELL_FS_S_IFREG      0x8000u
#define CELL_FS_O_RDONLY     0
#define CELL_FS_O_WRONLY     1
#define CELL_FS_O_RDWR       2
#define CELL_FS_O_CREAT      0x0200
#define CELL_FS_O_TRUNC      0x0400
#define CELL_FS_O_APPEND     0x0100
#define CELL_FS_SEEK_SET     0
#define CELL_FS_SEEK_CUR     1
#define CELL_FS_SEEK_END     2
#define CELL_FS_TYPE_DIR     1
#define CELL_FS_TYPE_REG     2

/* ---- guest memory string helpers ---- */
static void guest_strcpy(char* dst, uint32_t gaddr, size_t cap)
{
    size_t i = 0;
    for (; i < cap - 1; i++) {
        if (ppu_vm_size && gaddr + i >= ppu_vm_size) break;
        char c = (char)vm_base[gaddr + i];
        if (!c) break;
        dst[i] = c;
    }
    dst[i] = 0;
}

/* Defined in runtime/syscalls/sys_fs.c -- shared by all three path
 * translators. (extern "C" is ill-formed at block scope.) */
extern "C" void ps3_vfs_ps3game_fallback(char* path, size_t cap);

/* Translate a guest path to a host path under ppu_vfs_root. Known PS3 mount
 * prefixes are stripped; the rest is appended to the root. */
static void host_path(char* out, size_t cap, const char* guest)
{
    const char* rel = guest;
    /* /dev_hdd0 overlays the installed game-update dir (a disc title patched to
     * e.g. v1.30 runs the update's EBOOT and reads its patchN.farc from
     * /dev_hdd0/game/<title>/), mirroring the PS3/RPCS3 layout: base data on
     * /dev_bdvd (disc), update data on /dev_hdd0. Set PS3_HDD0_ROOT to the host
     * dir that /dev_hdd0 maps into (the one containing game/<title>/). */
    static const char* hdd0_root = nullptr; static int hdd0_init = 0;
    if (!hdd0_init) { hdd0_root = getenv("PS3_HDD0_ROOT"); hdd0_init = 1; }
    if (hdd0_root && strncmp(guest, "/dev_hdd0/", 10) == 0) {
        snprintf(out, cap, "%s/%s", hdd0_root, guest + 10);
        for (char* p = out; *p; p++) if (*p == '\\') *p = '/';
        return;
    }

    /* /dev_flash is FIRMWARE, not game data — mapping it into the game root (the
     * old behaviour) makes every firmware lookup miss. YDKJ's FMOD asks for
     * /dev_flash/sys/external/flashMP3.pic and, on a miss, prints "mp3 failed to
     * load MP3 codec (are you using the correct flash?)" and then crashes.
     * Serve it from a real dev_flash tree instead ($PS3_DEV_FLASH, else RPCS3's). */
    if (strncmp(guest, "/dev_flash/", 11) == 0) {
        const char* fw = getenv("PS3_DEV_FLASH");
        if (!fw || !*fw) fw = "D:/recomp/tools/rpcs3/dev_flash";
        snprintf(out, cap, "%s/%s", fw, guest + 11);
        for (char* p = out; *p; p++) if (*p == '\\') *p = '/';
        return;
    }

    static const char* mounts[] = {
        "/dev_bdvd/", "/app_home/", "/dev_hdd0/", "/dev_hdd1/",
        "/dev_flash/", "/host_root/", "/dev_usb000/", "/dev_usb/"
    };
    for (size_t i = 0; i < sizeof(mounts)/sizeof(mounts[0]); i++) {
        size_t n = strlen(mounts[i]);
        if (strncmp(guest, mounts[i], n) == 0) { rel = guest + n; break; }
        /* Every mount above ends in '/', so a BARE device root never matched
         * its own mount: "/dev_bdvd" is not a prefix-match for "/dev_bdvd/".
         * It fell through to the strip-leading-slash case and resolved to
         * <root>/dev_bdvd, which does not exist -- so a title that opendirs or
         * stats the device to check the medium is present reads it as "no
         * disc". YDKJ does exactly that, 27 times a boot, and consequently
         * loaded none of its Scaleform UI or FMOD banks: no logos, no legal
         * screen, just a clear colour.
         *
         * libs/filesystem/cellFs.c already carries this fix (Tokyo Jungle hit
         * the same wall and retried forever); ppu_fs is the other half of the
         * split filesystem and never got it. Map the bare root to the mount
         * directory itself. */
        if (n > 1 && strncmp(guest, mounts[i], n - 1) == 0 && guest[n - 1] == '\0') {
            rel = guest + n - 1;   /* points at the NUL -> <root>/ */
            break;
        }
    }
    if (rel == guest && guest[0] == '/') rel = guest + 1;   /* strip leading '/' */
    snprintf(out, cap, "%s/%s", ppu_vfs_root, rel);
    for (char* p = out; *p; p++) if (*p == '\\') *p = '/';
    ps3_vfs_ps3game_fallback(out, cap);
}

/* ---- fd / dir handle tables ---- */
#define FS_MAX 256
using HostFile = ps3recomp::host::File;
using HostFilePtr = std::shared_ptr<HostFile>;
struct FsFileEntry {
    HostFilePtr file;
    bool usm = false;
    bool cutscene = false;
    char path[192] = {};
};
struct FsDirEntry {
    DIR* directory = nullptr;
    char path[1024] = {};
    std::mutex io_mutex;
    ~FsDirEntry()
    {
        if (directory)
            closedir(directory);
    }
};
using FsFileEntryPtr = std::shared_ptr<FsFileEntry>;
using FsDirEntryPtr = std::shared_ptr<FsDirEntry>;
static FsFileEntryPtr g_files[FS_MAX];
static std::mutex g_fd_table_mutex;
/* Story-cutscene movies currently open (inFamous: BIK files under movies/cutscene other than the startup splash/logo
 * videos). The file stays open for exactly as long as the movie plays; rsx_ui_overlay.c hides the pause
 * watermark while this is non-zero, so it shows in menus and startup videos but not in cutscenes. */
extern "C" volatile int g_fs_cutscene_open = 0;
static int fs_is_story_cutscene(const char* gpath)
{
    const char* d = strstr(gpath, "/movies/cutscene/");
    if (!d) return 0;
    const char* name = d + 17;
    if (!strstr(name, ".bik")) return 0;
    return strncmp(name, "splash_", 7) != 0 && strncmp(name, "infamous_logo", 13) != 0;
}
static FsDirEntryPtr g_dirs[FS_MAX];

static FsFileEntryPtr fd_file(int fd)
{
    if (fd < 0 || fd >= FS_MAX) return nullptr;
    std::lock_guard<std::mutex> lock(g_fd_table_mutex);
    return g_files[fd];
}
static FsFileEntryPtr fd_detach_file(int fd)
{
    if (fd < 0 || fd >= FS_MAX) return nullptr;
    std::lock_guard<std::mutex> lock(g_fd_table_mutex);
    FsFileEntryPtr entry = std::move(g_files[fd]);
    if (entry && entry->cutscene)
        InterlockedDecrement((volatile LONG*)&g_fs_cutscene_open);
    return entry;
}
static FsDirEntryPtr fd_dir(int fd)
{
    if (fd < 0 || fd >= FS_MAX) return nullptr;
    std::lock_guard<std::mutex> lock(g_fd_table_mutex);
    return g_dirs[fd];
}
static FsDirEntryPtr fd_detach_dir(int fd)
{
    if (fd < 0 || fd >= FS_MAX) return nullptr;
    std::lock_guard<std::mutex> lock(g_fd_table_mutex);
    return std::move(g_dirs[fd]);
}
static int fd_alloc_file(FsFileEntryPtr entry)
{
    std::lock_guard<std::mutex> lock(g_fd_table_mutex);
    for (int i = 3; i < FS_MAX; i++) {
        if (!g_files[i] && !g_dirs[i]) {
            g_files[i] = std::move(entry);
            if (g_files[i]->cutscene)
                InterlockedIncrement((volatile LONG*)&g_fs_cutscene_open);
            return i;
        }
    }
    return -1;
}
static int fd_alloc_dir(FsDirEntryPtr entry)
{
    std::lock_guard<std::mutex> lock(g_fd_table_mutex);
    for (int i = 3; i < FS_MAX; i++)
        if (!g_files[i] && !g_dirs[i]) { g_dirs[i] = std::move(entry); return i; }
    return -1;
}

static ps3recomp::host::OpenOptions host_open_options(uint32_t flags)
{
    ps3recomp::host::OpenOptions options;
    const uint32_t access = flags & 0x3;
    options.access = access == CELL_FS_O_RDWR
        ? ps3recomp::host::FileAccess::read_write
        : access == CELL_FS_O_WRONLY
            ? ps3recomp::host::FileAccess::write_only
            : ps3recomp::host::FileAccess::read_only;
    options.create = (flags & CELL_FS_O_CREAT) != 0;
    options.truncate = (flags & CELL_FS_O_TRUNC) != 0;
    options.append = (flags & CELL_FS_O_APPEND) != 0;
    return options;
}

/* ---- handlers ---- */
static void cellFsOpen(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t flags  = (uint32_t)ctx->gpr[4];
    uint32_t fd_ptr = (uint32_t)ctx->gpr[5];
    host_path(hpath, sizeof hpath, gpath);

    /* NPDRM: see the note in sys_fs.c -- an EDAT is decrypted once into a cache
     * file and that is opened in its place. */
    { char dec_path[1200];
      const char* use = edat_resolve(hpath, dec_path, sizeof dec_path);
      if (use != hpath) snprintf(hpath, sizeof hpath, "%s", use); }

    /* A bare device root ("/dev_bdvd") resolves to a DIRECTORY, and open() on a
     * directory fails on Windows. Real cellFsOpen answers EISDIR there, and a
     * title reads that as "the medium is mounted" -- YDKJ probes the disc this
     * way before loading its Scaleform UI and FMOD banks, so the ENOENT it got
     * instead read as "no disc" and it drew nothing but a clear colour.
     * S_IFMT/S_IFDIR rather than S_ISDIR: MSVC's CRT does not define the macro. */
    {
        struct stat _dst;
        if (stat(hpath, &_dst) == 0 && (_dst.st_mode & S_IFMT) == S_IFDIR) {
            ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EISDIR;
            return;
        }
    }
    int host_error = 0;
    HostFilePtr file = HostFile::open(hpath, host_open_options(flags), &host_error);
    if (!file) {
        fprintf(stderr, "[fs] open FAIL '%s' -> '%s' (host error %d)\n",
                gpath, hpath, host_error);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return;
    }
    auto entry = std::make_shared<FsFileEntry>();
    entry->file = std::move(file);
    entry->usm = strstr(gpath, ".usm") != nullptr;
    entry->cutscene = fs_is_story_cutscene(gpath) != 0;
    strncpy(entry->path, gpath, sizeof(entry->path) - 1);
    int fd = fd_alloc_file(entry);
    if (fd < 0) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    /* FS_CALLER=1: name the guest function that opened each file. A title whose
     * streaming layer opens a file and never reads it (You Don't Know Jack's
     * movies) is diagnosed from the opener: its sibling read path is the one
     * that is not running. */
    { char who[64] = "?";
      if (getenv("FS_CALLER")) { ppu_guest_caller(who, sizeof who);
          fprintf(stderr, "[fs] open '%s' -> fd %d  (opened by %s)\n", gpath, fd, who);
          /* FS_STACK=<substr>: every open funnels through one guest wrapper, so
           * one caller level says nothing about WHICH subsystem wanted the file.
           * Dump the guest call chain for the opens that matter. */
          { const char* want = getenv("FS_STACK");
            if (want && *want && strstr(gpath, want)) ydkj_host_bt("fs-open");
          } }
      else if (getenv("FS_OPEN_LOG"))
          fprintf(stderr, "[fs] open '%s' -> fd %d\n", gpath, fd); }
    if (getenv("PS3_FSLOG_BT") && strstr(gpath, ".usm")) {
        /* Resolve to GUEST functions (raw host RVAs are useless here): this tells us
         * which criMv/criFs function opened the movie, so the reader-attach path
         * (stream+0x10, never wired => movie opened but never read) can be found. */
        fprintf(stderr,"[USMBT] open '%s' -> fd %d\n", gpath, fd); fflush(stderr);
        ydkj_host_bt("usm-open");
    }
    ctx->gpr[3] = CELL_OK;
}

/* cellFsSdataOpen(path, flags, fd*, arg*, size): opens a PS3 SDATA/EDAT
 * encrypted-data file and returns an fd whose reads yield the DECRYPTED
 * plaintext. LBP 1.30 stores its SPU job code modules inside patch.sdat (an
 * FSHb container, zlib-compressed C0DEC0DE modules) and opens it via this call;
 * leaving it faked (default CELL_OK, no handle) meant the container was never
 * read -> zero job modules loaded -> every SPU physics job dispatched into an
 * empty code buffer -> the loading freeze.
 *
 * Our run env's PS3_HDD0_ROOT (RPCS3 dev_hdd0) already holds patch.sdat in
 * DECRYPTED form (magic "FSHb", not "NPD"), so we just open it read-only --
 * no EDAT decryption needed. (A title supplying a still-encrypted NPD .sdat
 * would need the SDATA block cipher; not required here.) The license `arg`
 * (gpr[6]) and its size (gpr[7]) are ignored. */
static void cellFsSdataOpen(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t fd_ptr = (uint32_t)ctx->gpr[5];
    host_path(hpath, sizeof hpath, gpath);

    /* Same NPDRM step cellFsOpen does: an encrypted container is decrypted once
     * into a cache file and that is opened in its place. This used to be a
     * second, weaker decryptor local to this function (sdata_decrypt below),
     * which handled the self-keyed SDATA form only and refused every real EDAT
     * -- so a title whose data file was EDAT rather than SDATA got "success
     * without a handle" here while the very same file opened fine through
     * cellFsOpen. One decryptor, reached from both doors. */
    { char dec_path[1200];
      const char* use = edat_resolve(hpath, dec_path, sizeof dec_path);
      if (use != hpath) snprintf(hpath, sizeof hpath, "%s", use); }

    int host_error = 0;
    HostFilePtr file = HostFile::open(
        hpath, {ps3recomp::host::FileAccess::read_only, false, false, false},
        &host_error);
    if (!file) {
        fprintf(stderr, "[fs] SdataOpen FAIL '%s' -> '%s' (host error %d)\n",
                gpath, hpath, host_error);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return;
    }
    /* If the file IS still NPD-encrypted, decrypt it in full and serve the
     * plaintext from a native temporary handle. */
    unsigned char magic[4] = {0};
    size_t got = file->read(magic, sizeof magic);
    file->seek(0, SEEK_SET);
    if (got == 4 && magic[0]=='N' && magic[1]=='P' && magic[2]=='D') {
        const uint64_t enc_sz = file->size();
        uint8_t* enc = (enc_sz > 0 && enc_sz <= SIZE_MAX)
            ? (uint8_t*)malloc((size_t)enc_sz) : nullptr;
        file->seek(0, SEEK_SET);
        size_t rd = enc ? file->read(enc, (size_t)enc_sz) : 0;
        file.reset();
        size_t dec_sz = 0;
        uint8_t* dec = (enc && rd == (size_t)enc_sz) ? sdata_decrypt(enc, rd, &dec_sz) : nullptr;
        free(enc);
        if (!dec) {
            fprintf(stderr, "[fs] SdataOpen '%s' NPD decrypt FAILED (unsupported "
                    "EDAT/needs license); returning success without a handle\n", gpath);
            ctx->gpr[3] = CELL_OK; return;   /* don't feed the title ciphertext */
        }
        char dmagic[4] = {0};
        if (dec_sz >= 4) memcpy(dmagic, dec, 4);
        HostFilePtr temporary = HostFile::temporary(&host_error);
        if (!temporary || temporary->write(dec, dec_sz) != dec_sz ||
            !temporary->seek(0, SEEK_SET)) {
            free(dec);
            ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return;
        }
        free(dec);
        auto entry = std::make_shared<FsFileEntry>();
        entry->file = std::move(temporary);
        int fd = fd_alloc_file(entry);
        if (fd < 0) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
        if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
        fprintf(stderr, "[fs] SdataOpen '%s' -> fd %d (NPD decrypted, 0x%zX bytes, magic '%c%c%c%c')\n",
                gpath, fd, dec_sz,
                dmagic[0]?dmagic[0]:'?', dmagic[1]?dmagic[1]:'?',
                dmagic[2]?dmagic[2]:'?', dmagic[3]?dmagic[3]:'?');
        ctx->gpr[3] = CELL_OK;
        return;
    }
    auto entry = std::make_shared<FsFileEntry>();
    entry->file = std::move(file);
    strncpy(entry->path, gpath, sizeof(entry->path) - 1);
    int fd = fd_alloc_file(entry);
    if (fd < 0) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    fprintf(stderr, "[fs] SdataOpen '%s' -> fd %d (magic %c%c%c%c)\n", gpath, fd,
            magic[0]?magic[0]:'?', magic[1]?magic[1]:'?', magic[2]?magic[2]:'?', magic[3]?magic[3]:'?');
    ctx->gpr[3] = CELL_OK;
}

static void cellFsClose(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    if (getenv("FS_CALLER")) { char w[64]="?"; ppu_guest_caller(w,sizeof w);
        fprintf(stderr, "[fs] close fd=%d  (by %s)\n", fd, w); }
    FsFileEntryPtr entry = fd_detach_file(fd);
    if (entry && entry->usm && getenv("PS3_FSLOG_READS"))
        fprintf(stderr, "[USMRD] CLOSE usm fd=%d\n", fd);
    ctx->gpr[3] = CELL_OK;
}

/* Pre-fault a guest range so the kernel can write into it.
 *
 * The flat VM is MEM_RESERVEd and each page is committed on FIRST ACCESS by a
 * vectored exception handler. That covers CPU access from lifted code, but
 * host reads/writes move data through the kernel, and a kernel write to a reserved
 * page does not raise a user-mode exception -- the I/O just fails, returning 0
 * with a host error. Twisted Metal hit this on every read whose destination the
 * guest had not touched yet: read 1 of a file succeeded, read 2 into a fresh
 * page came back n=0 eof=0 err=1, and the title logged its own
 * "Short read ... Possible reasons include disc eject" and gave up.
 *
 * Touch each page read-then-write so the fault happens here, in user mode,
 * where the handler can commit it. Read-then-write rather than a plain store so
 * nothing already in the buffer is disturbed. */
static inline void fs_prefault(uint32_t buf, uint64_t len)
{
    if (!vm_base || !len) return;
    volatile uint8_t* p = (volatile uint8_t*)(vm_base + buf);
    for (uint64_t o = 0; o < len; o += 0x1000) p[o] = p[o];
    p[len - 1] = p[len - 1];
}

static void cellFsRead(ppu_context* ctx)
{
    const auto phase_start = std::chrono::steady_clock::now();
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t buf    = (uint32_t)ctx->gpr[4];
    uint64_t nbytes = ctx->gpr[5];
    uint32_t nread_ptr = (uint32_t)ctx->gpr[6];
    FsFileEntryPtr entry = fd_file(fd);
    const auto phase_lookup = std::chrono::steady_clock::now();
    if (!entry) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    HostFilePtr file = entry->file;
    uint64_t raw_nbytes = ctx->gpr[5];
    long fpos_before = (long)file->position();
    const auto phase_position = std::chrono::steady_clock::now();
    if (ppu_vm_size && (uint64_t)buf + nbytes > ppu_vm_size) nbytes = ppu_vm_size - buf;
    fs_prefault(buf, nbytes);
    const auto phase_prefault = std::chrono::steady_clock::now();
#if PS3_HZ_TELEMETRY()
    const unsigned long long _hz0 = hz_tsc();   /* v225A */
#endif
    size_t n = file->read(vm_base + buf, (size_t)nbytes);   /* raw bytes, no swap */
    const auto phase_read = std::chrono::steady_clock::now();
#if PS3_HZ_TELEMETRY()
    { const long long _hzd = (long long)(hz_tsc() - _hz0); hz_add(HZ_FS_TSC, _hzd); hz_add(HZ_FS_N, 1); hz_add(HZ_FS_BYTES, (long long)n);
      if (ps3_hz_is_main()) { hz_add(HZ_FS_MAIN_TSC, _hzd); hz_add(HZ_FS_MAIN_N, 1); } }
#endif
    /* v114 FS_READ_DELAY_MS=<ms> (default 0 = off): CAUSAL TEST, not a fix. Sleep after every exactly-64 KB
     * read (the FIOS world-streaming chunks of infamous1.psarc_s; loading reads are ~490 KB and unaffected).
     * If the world-movement hitches get longer by about the added burst time, streaming speed causes them. */
    { static long dms = -1;
      if (dms < 0) { const char* e = getenv("FS_READ_DELAY_MS"); dms = e ? atol(e) : 0; if (dms < 0) dms = 0;
                     if (dms) fprintf(stderr, "[fs] v114 CAUSAL TEST: +%ld ms after every 64 KB read (FS_READ_DELAY_MS)\n", dms); }
      if (dms && nbytes == 65536) std::this_thread::sleep_for(std::chrono::milliseconds(dms)); }
    /* PS3_FSLOG_EOF=<fd>,<bytes>: report end-of-file past <bytes> on one descriptor.
     * The intro cinematic is 28 seconds and this port renders it at a few frames
     * a second, so it cannot be watched to its end -- truncating the stream makes
     * the demuxer see EOF and the movie finish, which is what advances the title
     * to whatever follows the intro. Deliberately a testing knob, not a fix. */
    { static int efd = -2; static long elim = 0;
      if (efd == -2) { const char* e = getenv("PS3_FSLOG_EOF");
          if (e) { efd = atoi(e); const char* c = strchr(e, 44); elim = c ? atol(c + 1) : 0; }
          else efd = -1; }
      if (efd >= 0 && fd == efd && elim > 0 && fpos_before >= elim) {
          static int once = 0;
          if (!once++) fprintf(stderr, "[fs] PS3_FSLOG_EOF: fd=%d truncated at %ld bytes\n", fd, elim);
          n = 0;
      } }
    if (entry->usm && getenv("PS3_FSLOG_READS")) fprintf(stderr, "[USMRD] READ usm fd=%d nbytes=%llu -> %zu magic=%02X%02X%02X%02X pos=%ld lr=0x%08X\n", fd, (unsigned long long)nbytes, n, vm_base[buf], vm_base[buf+1], vm_base[buf+2], vm_base[buf+3], fpos_before, (uint32_t)ctx->lr);
    /* PS3_FSLOG_READS=<fd>: log every read on one descriptor. PS3_FSLOG caps at 20
     * lines and they are all spent before a movie ever opens, so it cannot
     * answer "is the streamer reading the .avi". */
    { static int wfd = -2;
      if (wfd == -2) { const char* e = getenv("PS3_FSLOG_READS"); wfd = e ? atoi(e) : -1; }
      if (wfd >= 0 && fd == wfd)
          fprintf(stderr, "[fsread] fd=%d want=%llu got=%zu pos=%ld\n",
                  fd, (unsigned long long)nbytes, n, fpos_before); }
    if (getenv("PS3_FSLOG")) { static int _fd=0; if((_fd < 20 && _fd++ < 20)) fprintf(stderr,"[FSDBG] fd=%d raw_nbytes=0x%llX clamped=0x%llX buf=0x%08X fpos_before=%ld n=%zu host_err=%d\n", fd,(unsigned long long)raw_nbytes,(unsigned long long)nbytes,buf,fpos_before,n,file->last_host_error()); }
#ifdef _WIN32
    if (getenv("PS3_FSLOG") && buf==0 && raw_nbytes>0x10000) { static int _b=0; if((_b < 2 && _b++ < 2)){ void* fr[30]; unsigned short nn=RtlCaptureStackBackTrace(0,30,fr,0); uintptr_t mb=(uintptr_t)GetModuleHandleA(0); fprintf(stderr,"[FSBT] null-buf read caller rvas:"); for(unsigned short i=0;i<nn&&i<16;i++) fprintf(stderr," %llX",(unsigned long long)((uintptr_t)fr[i]-mb)); fprintf(stderr,"\n"); } }
#endif
    /* Per-fd totals, not just the first 50 lines. The flat cap made "this file is
     * opened and never read" unfalsifiable: reads on a later-opened fd fall off
     * the end of the log and look identical to reads that never happen.
     * FS_READ_ALL=1 logs every read; otherwise a per-fd first-read line plus a
     * periodic summary is enough to tell the two apart. */
    { static uint64_t tot=0, read_count=0; static int _n=0;
      static uint64_t per_fd[64]; static uint32_t cnt_fd[64];
      tot+=n;
      read_count++;
      if (fd>=0 && fd<64) { per_fd[fd]+=n; cnt_fd[fd]++; }
      const bool read_log = getenv("FS_READ_LOG") != nullptr;
      if (read_log) {
          const int first_for_fd = (fd>=0 && fd<64 && cnt_fd[fd]==1);
          if ((_n < 50 && _n++ < 50) || first_for_fd || getenv("FS_READ_ALL"))
              fprintf(stderr,"[fs] read fd=%d nbytes=%llu -> %zu (magic=%02X%02X%02X%02X, total=%llu)%s\n",
                      fd,(unsigned long long)nbytes,n,vm_base[buf],vm_base[buf+1],vm_base[buf+2],vm_base[buf+3],
                      (unsigned long long)tot, first_for_fd?"  <= FIRST READ ON THIS FD":"");
          if ((read_count % 2000)==0) { fprintf(stderr,"[fs] read summary after %llu reads:",(unsigned long long)read_count);
              for (int i=0;i<64;i++) if (cnt_fd[i]) fprintf(stderr," fd%d=%ux/%lluB",i,cnt_fd[i],(unsigned long long)per_fd[i]);
              fprintf(stderr,"\n");
          }
      }
      if (getenv("FS_READ_PATH") && fd>=0 && fd<64 && entry->path[0])
          fprintf(stderr, "        from %s\n", entry->path);
    }
    if (getenv("PS3_FSLOG_TOC") && nbytes >= 50000) {  /* data.toc read -> who parses it? */
        fprintf(stderr, "[TOC] data.toc read into buf=0x%08X n=%zu; lr=0x%08llX; guest-stack RAs:\n", buf, n, (unsigned long long)ctx->lr);
        uint32_t sp = (uint32_t)ctx->gpr[1];
        for (uint32_t i = 0; i < 128 && sp + i*4 + 4 <= ppu_vm_size; i++) {
            uint32_t a = sp + i*4; uint32_t w = (vm_base[a]<<24)|(vm_base[a+1]<<16)|(vm_base[a+2]<<8)|vm_base[a+3];
            if (w >= 0x10000u && w < 0x600000u) fprintf(stderr, "[TOC]   ra 0x%08X (@sp+0x%X)\n", w, i*4);
        }
    }
    const auto phase_end = std::chrono::steady_clock::now();
    auto ms = [](auto from, auto to) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
    };
    if (ms(phase_start, phase_end) >= 100) {
        fprintf(stderr,
                "[FSPHASE] tid=%llu fd=%d want=%llu got=%zu total=%lldms "
                "lookup=%lld position=%lld prefault=%lld read=%lld post=%lld\n",
                (unsigned long long)ctx->thread_id, fd,
                (unsigned long long)raw_nbytes, n,
                (long long)ms(phase_start, phase_end),
                (long long)ms(phase_start, phase_lookup),
                (long long)ms(phase_lookup, phase_position),
                (long long)ms(phase_position, phase_prefault),
                (long long)ms(phase_prefault, phase_read),
                (long long)ms(phase_read, phase_end));
    }
    if (nread_ptr) vm_write64(nread_ptr, n);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsWrite(ppu_context* ctx)
{
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t buf    = (uint32_t)ctx->gpr[4];
    uint64_t nbytes = ctx->gpr[5];
    uint32_t nwr_ptr = (uint32_t)ctx->gpr[6];
    FsFileEntryPtr entry = fd_file(fd);
    if (!entry) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    HostFilePtr file = entry->file;
    fs_prefault(buf, nbytes);   /* kernel READS the buffer; same reserved-page trap */
    size_t n = file->write(vm_base + buf, (size_t)nbytes);
    /* DIAGNOSTIC (PS3_FSLOG_BT=1): dump the guest back-chain when the game logs the
     * render-config failure, to locate setScreenRenderTargetInternal & the config obj. */
    if (getenv("PS3_FSLOG_BT") && buf && nbytes > 0 && nbytes < 4096 && vm_base) {
        char tmp[256]; uint32_t nn = (uint32_t)(nbytes < 255 ? nbytes : 255);
        memcpy(tmp, vm_base + buf, nn); tmp[nn] = 0;
        if (strstr(tmp,"config") || strstr(tmp,"Config") || strstr(tmp,"Mystery") ||
            strstr(tmp,"downsample") || strstr(tmp,"RenderTarget")) {
            uint32_t sp = (uint32_t)ctx->gpr[1];
            fprintf(stderr, "[cfgbt] write \"%.60s\" lr=0x%08X sp=0x%08X\n", tmp, (uint32_t)ctx->lr, sp);
            for (int i = 0; i < 24 && sp && sp < 0x10000000u; i++) {
                uint32_t nsp; memcpy(&nsp, vm_base + sp, 4);
                nsp = ((nsp>>24)&0xFF)|((nsp>>8)&0xFF00)|((nsp<<8)&0xFF0000)|((nsp<<24)&0xFF000000);
                if (nsp <= sp || nsp >= 0x10000000u) break;
                uint32_t lr; memcpy(&lr, vm_base + nsp + 0x10, 4);
                lr = ((lr>>24)&0xFF)|((lr>>8)&0xFF00)|((lr<<8)&0xFF0000)|((lr<<24)&0xFF000000);
                fprintf(stderr, "[cfgbt]   #%d lr=0x%08X\n", i, lr);
                sp = nsp;
            }
            fflush(stderr);
        }
    }
    if (nwr_ptr) vm_write64(nwr_ptr, n);
    ctx->gpr[3] = CELL_OK;
}

/* cellFsFGetBlockSize(int fd, u64* sector_size, u64* block_size)
 * cellFsGetBlockSize(const char* path, u64* sector_size, u64* block_size)
 *
 * Titles ask the filesystem what granularity it wants and then size and ALIGN
 * their reads to the answer. Left unregistered, the call fell through to the
 * unresolved-NID path: the out-params were never written, so the title read
 * whatever was already in them.
 *
 * The Orange Box's chooser does exactly this. With no answer it re-seeks to
 * offset 0 and re-reads the same 64 KB of APP_CHOOSER.GRP forever -- 241,997
 * reads and 15.86 GB of I/O against a 4 MB file in one run -- because the
 * offset arithmetic it derives from the block size collapses to zero.
 *
 * Both out-params are u64 and big-endian, so they go through vm_write64.
 * PS3_FS_BLOCK_SIZE overrides the value for a title that wants a different
 * granularity. */
static void fs_report_block_size(ppu_context* ctx, uint32_t sec_ptr, uint32_t blk_ptr)
{
    static uint64_t bs = 0;
    if (!bs) {
        const char* e = getenv("PS3_FS_BLOCK_SIZE");
        bs = (e && *e) ? (uint64_t)strtoull(e, nullptr, 0) : 4096ull;
        if (!bs) bs = 4096ull;
    }
    if (sec_ptr) vm_write64(sec_ptr, bs);
    if (blk_ptr) vm_write64(blk_ptr, bs);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsFGetBlockSize(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    FsFileEntryPtr entry = fd_file(fd);
    if (!entry) {
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO;
        return;
    }
    fs_report_block_size(ctx, (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5]);
}

static void cellFsGetBlockSize(ppu_context* ctx)
{
    /* Path variant: the answer does not depend on the file, only the device,
     * and every mount we serve is backed by the same host filesystem. */
    fs_report_block_size(ctx, (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5]);
}

static void cellFsLseek(ppu_context* ctx)
{
    int fd        = (int)(uint32_t)ctx->gpr[3];
    int64_t off   = (int64_t)ctx->gpr[4];
    uint32_t wh   = (uint32_t)ctx->gpr[5];
    uint32_t pos_ptr = (uint32_t)ctx->gpr[6];
    FsFileEntryPtr entry = fd_file(fd);
    if (!entry) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    HostFilePtr file = entry->file;
    if (getenv("FS_CALLER")) { char w[64]="?"; ppu_guest_caller(w,sizeof w);
        fprintf(stderr, "[fs] lseek fd=%d off=%lld whence=%u  (by %s)\n", fd, (long long)off, wh, w); }
    int worigin = (wh == CELL_FS_SEEK_END) ? SEEK_END : (wh == CELL_FS_SEEK_CUR) ? SEEK_CUR : SEEK_SET;
    file->seek(off, worigin);
    const int64_t p = file->position();
    if (pos_ptr) vm_write64(pos_ptr, (uint64_t)p);
    ctx->gpr[3] = CELL_OK;
}

/* CellFsStat is 0x34 (52) bytes, 4-byte aligned -- the s64/u64 members are
 * be_t<...,4> so there is NO 4-byte pad after gid (verified vs RPCS3:
 * CHECK_SIZE_ALIGN(CellFsStat, 52, 4)). Laying it out 8-byte-aligned (0x38,
 * pad@0x0C) shifts size/blksize +4 and overruns the struct by 4 bytes -- games
 * that embed a CellFsStat inside a larger object (e.g. Dantelion's
 * DLFileDeviceStream, stat@obj+0xD8) then have the trailing blksize clobber the
 * field right after the stat (the fd at obj+0x10c), which later fails lseek.
 * Layout: mode@0 uid@4 gid@8 atime@0x0C mtime@0x14 ctime@0x1C size@0x24 blksize@0x2C. */
static void write_stat(uint32_t sb, uint32_t mode, uint64_t size)
{
    vm_write32(sb + 0x00, mode);
    vm_write32(sb + 0x04, 0);            /* uid */
    vm_write32(sb + 0x08, 0);            /* gid */
    vm_write64(sb + 0x0C, 0);            /* atime */
    vm_write64(sb + 0x14, 0);            /* mtime */
    vm_write64(sb + 0x1C, 0);            /* ctime */
    vm_write64(sb + 0x24, size);         /* size */
    vm_write64(sb + 0x2C, 0x200);        /* blksize */
}

static void cellFsStat(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t sb = (uint32_t)ctx->gpr[4];
    host_path(hpath, sizeof hpath, gpath);
    struct stat st;
    if (stat(hpath, &st) != 0) {
        if (getenv("PS3_FSLOG")) fprintf(stderr, "[fs] stat '%s' -> ENOENT\n", gpath);
        ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return;
    }
    if (getenv("PS3_FSLOG")) fprintf(stderr, "[fs] stat '%s' -> OK (size=%lld)\n", gpath, (long long)st.st_size);
    uint32_t mode = (st.st_mode & S_IFDIR) ? (CELL_FS_S_IFDIR | 0x1FF)
                                           : (CELL_FS_S_IFREG | 0x1B6);
    if (sb) write_stat(sb, mode, (uint64_t)st.st_size);
    if (getenv("PS3_FSLOG") && strstr(gpath,".toc")) fprintf(stderr,"[FSDBG] cellFsStat('%s') -> size=0x%llX\n",gpath,(unsigned long long)st.st_size);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsFstat(ppu_context* ctx)
{
    int fd      = (int)(uint32_t)ctx->gpr[3];
    uint32_t sb = (uint32_t)ctx->gpr[4];
    FsFileEntryPtr entry = fd_file(fd);
    if (!entry) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    HostFilePtr file = entry->file;
    if (getenv("FS_CALLER")) { char w[64]="?"; ppu_guest_caller(w,sizeof w);
        fprintf(stderr, "[fs] fstat fd=%d  (by %s)\n", fd, w); }
    uint64_t sz = file->size();
    /* FS_FSTAT_CAP=<bytes>: DIAGNOSTIC. Report a smaller size than the file
     * has. You Don't Know Jack reads a 100 KB archive whole but only probes
     * (open/fstat/close) its 500 KB and 2.4 MB ones; if that branch is driven
     * by the size it just asked for, capping it makes the title take the read
     * path. Answers whether the split is size-based -- nothing more. */
    { const char* cap = getenv("FS_FSTAT_CAP");
      const char* only = getenv("FS_FSTAT_CAP_PATH");
      if (cap && only && *only && !(entry->path[0] && strstr(entry->path, only))) cap = 0;
      if (cap) { long long c = strtoll(cap, 0, 0);
          if (c > 0 && sz > (uint64_t)c) {
              fprintf(stderr, "[fs] fstat fd=%d size %llu -> capped %lld (FS_FSTAT_CAP)\n",
                      fd, (unsigned long long)sz, c);
              sz = (uint64_t)c; } } }
    if (sb) write_stat(sb, CELL_FS_S_IFREG | 0x1B6, sz);
    if (getenv("PS3_FSLOG")) { static int _n=0; if((_n < 12 && _n++ < 12)) fprintf(stderr,"[FSDBG] cellFsFstat(fd=%d) -> size=0x%llX\n",fd,(unsigned long long)sz); }
    ctx->gpr[3] = CELL_OK;
}

static void cellFsOpendir(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    uint32_t fd_ptr = (uint32_t)ctx->gpr[4];
    host_path(hpath, sizeof hpath, gpath);
    DIR* d = opendir(hpath);
    if (getenv("PS3_FSLOG")) fprintf(stderr, "[fs] opendir '%s' -> %s\n", gpath, d ? "OK" : "ENOENT");
    if (!d) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_ENOENT; return; }
    auto entry = std::make_shared<FsDirEntry>();
    entry->directory = d;
    strncpy(entry->path, hpath, sizeof(entry->path) - 1);
    int fd = fd_alloc_dir(entry);
    if (fd < 0) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    if (fd_ptr) vm_write32(fd_ptr, (uint32_t)fd);
    ctx->gpr[3] = CELL_OK;
}

/* CellFsDirent: d_type(1) d_namlen(1) d_name[256]; total 0x102. */
static void cellFsReaddir(ppu_context* ctx)
{
    int fd          = (int)(uint32_t)ctx->gpr[3];
    uint32_t dirent = (uint32_t)ctx->gpr[4];
    uint32_t nread_ptr = (uint32_t)ctx->gpr[5];
    FsDirEntryPtr entry = fd_dir(fd);
    if (!entry) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    std::lock_guard<std::mutex> io_lock(entry->io_mutex);
    if (!entry->directory) { ctx->gpr[3] = (uint64_t)(int64_t)CELL_FS_EIO; return; }
    struct dirent* e = readdir(entry->directory);
    if (!e) { if (nread_ptr) vm_write64(nread_ptr, 0); ctx->gpr[3] = CELL_OK; return; }
    char full[1300]; struct stat st;
    snprintf(full, sizeof full, "%s/%s", entry->path, e->d_name);
    uint8_t type = (stat(full, &st) == 0 && (st.st_mode & S_IFDIR))
                   ? CELL_FS_TYPE_DIR : CELL_FS_TYPE_REG;
    size_t nl = strlen(e->d_name); if (nl > 255) nl = 255;
    vm_base[dirent + 0] = type;
    vm_base[dirent + 1] = (uint8_t)nl;
    for (size_t i = 0; i < nl; i++) vm_base[dirent + 2 + i] = (uint8_t)e->d_name[i];
    vm_base[dirent + 2 + nl] = 0;
    if (nread_ptr) vm_write64(nread_ptr, 0x102);
    ctx->gpr[3] = CELL_OK;
}

static void cellFsClosedir(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    FsDirEntryPtr entry = fd_detach_dir(fd);
    if (entry) {
        std::lock_guard<std::mutex> io_lock(entry->io_mutex);
        closedir(entry->directory);
        entry->directory = nullptr;
    }
    ctx->gpr[3] = CELL_OK;
}

#ifdef _WIN32
#include <direct.h>
#define HOST_MKDIR(p) _mkdir(p)
#else
#define HOST_MKDIR(p) mkdir((p), 0777)
#endif

/* Create `path` and any missing parent dirs on the host. Returns 0 on
 * success or if the directory already exists. */
static int host_mkdir_p(const char* path)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    while (len > 1 && (tmp[len-1] == '/' || tmp[len-1] == '\\')) tmp[--len] = 0;
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') { char c = *p; *p = 0; HOST_MKDIR(tmp); *p = c; }
    }
    int r = HOST_MKDIR(tmp);
    if (r != 0) { struct stat st; if (stat(tmp, &st) == 0 && (st.st_mode & S_IFDIR)) r = 0; }
    return r;
}

/* cellFsMkdir(path, mode): create the guest directory on the host. A no-op stub
 * here silently breaks games that create then poll a dir (e.g. LBP's boot waits
 * for /dev_hdd0/game/<title>/USRDIR to appear). Create parents too. */
static void cellFsMkdir(ppu_context* ctx)
{
    char gpath[1024], hpath[1100];
    guest_strcpy(gpath, (uint32_t)ctx->gpr[3], sizeof gpath);
    host_path(hpath, sizeof hpath, gpath);
    int r = host_mkdir_p(hpath);
    if (getenv("PS3_FSLOG")) fprintf(stderr, "[fs] mkdir '%s' -> '%s' (%s)\n", gpath, hpath, r == 0 ? "OK" : "FAIL");
    ctx->gpr[3] = CELL_OK;   /* existing dir is benign for boot */
}
static void cellFsRmdir(ppu_context* ctx)  { ctx->gpr[3] = CELL_OK; }
static void cellFsUnlink(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }
static void cellFsFsync(ppu_context* ctx)
{
    int fd = (int)(uint32_t)ctx->gpr[3];
    FsFileEntryPtr entry = fd_file(fd);
    if (entry) entry->file->flush();
    ctx->gpr[3] = CELL_OK;
}
/* File-area preallocation (LBP cache warm-up spams this): host filesystems
 * grow files on write, so accepting is correct -- no preallocation needed. */
static void cellFsAllocateFileAreaWithoutZeroFill(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }

/* ---- cellFs AIO -----------------------------------------------------------
 *
 * Scott Pilgrim loads everything through cellFsAioRead: it opens gamedata.fat
 * with cellFsOpen and then never calls cellFsRead again. With the AIO NIDs
 * unresolved, its loader thread submitted requests that nothing ever completed
 * and the whole boot parked -- one open, no reads, and the main thread spinning.
 *
 * CellFsAio, big-endian, 0x28 bytes:
 *   0x00 u32 fd    0x08 u64 offset    0x10 u32 buf    0x18 u64 size    0x20 u64 user_data
 *
 * The completion callback is (CellFsAio* aio, s32 error, s32 id, u64 size).
 *
 * ponytail: the read runs synchronously and the callback fires before the
 * submit call returns. A title that submits, THEN arms the thing the callback
 * signals, would miss it -- move completion to a worker thread if one shows up.
 */
static uint32_t guest_be32(uint32_t a)
{
    if (ppu_vm_size && (uint64_t)a + 4 > ppu_vm_size) return 0;
    return ((uint32_t)vm_base[a] << 24) | ((uint32_t)vm_base[a+1] << 16) |
           ((uint32_t)vm_base[a+2] << 8) | (uint32_t)vm_base[a+3];
}
static uint64_t guest_be64(uint32_t a)
{
    return ((uint64_t)guest_be32(a) << 32) | guest_be32(a + 4);
}

static void cellFsAioInit(ppu_context* ctx)   { ctx->gpr[3] = CELL_OK; }
static void cellFsAioFinish(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }
/* Nothing is ever outstanding, so a cancel has nothing to find. Real cellFs
 * answers CELL_FS_ENOENT for an unknown id and callers treat that as "already
 * done", which is exactly true here. */
static void cellFsAioCancel(ppu_context* ctx) { ctx->gpr[3] = CELL_OK; }

static void cellFsAioRead(ppu_context* ctx)
{
    uint32_t aio    = (uint32_t)ctx->gpr[3];
    uint32_t id_ptr = (uint32_t)ctx->gpr[4];
    uint32_t cb_opd = (uint32_t)ctx->gpr[5];

    int      fd     = (int)guest_be32(aio + 0x00);
    uint64_t offset = guest_be64(aio + 0x08);
    uint32_t buf    = guest_be32(aio + 0x10);
    uint64_t size   = guest_be64(aio + 0x18);

    static std::atomic<int32_t> s_next_id{1};
    int32_t id = s_next_id.fetch_add(1, std::memory_order_relaxed);
    if (id_ptr) vm_write32(id_ptr, (uint32_t)id);

    size_t n = 0;
    int32_t err = CELL_FS_ENOENT;
    FsFileEntryPtr entry = fd_file(fd);
    if (entry) {
        HostFilePtr file = entry->file;
        if (ppu_vm_size && (uint64_t)buf + size > ppu_vm_size) size = ppu_vm_size - buf;
        fs_prefault(buf, size);
        /* AIO reads are absolute -- they do not disturb the fd's own file
         * position, and the guest interleaves them freely across threads. */
#if PS3_HZ_TELEMETRY()
        const unsigned long long _hz0 = hz_tsc();   /* v225A */
#endif
        n = file->read_at(offset, vm_base + buf, (size_t)size);
#if PS3_HZ_TELEMETRY()
        { const long long _hzd = (long long)(hz_tsc() - _hz0); hz_add(HZ_FS_TSC, _hzd); hz_add(HZ_FS_N, 1); hz_add(HZ_FS_BYTES, (long long)n);
          if (ps3_hz_is_main()) { hz_add(HZ_FS_MAIN_TSC, _hzd); hz_add(HZ_FS_MAIN_N, 1); } }
#endif
        err = CELL_OK;
    }
    if (getenv("PS3_FSLOG"))
        fprintf(stderr, "[fs] aio read id=%d fd=%d off=%llu size=%llu -> %zu\n",
                id, fd, (unsigned long long)offset, (unsigned long long)size, n);

    ctx->gpr[3] = CELL_OK;
    if (cb_opd) ps3_invoke_guest(cb_opd, aio, (uint64_t)(int64_t)err,
                                 (uint64_t)(int64_t)id, (uint64_t)n, 0, 0, 0, 0);
}

extern "C" void ppu_fs_register(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("cellFsOpen"),     "cellFsOpen",     cellFsOpen);
    /* Register by the literal import NID: LBP imports 0xB1840B53 for
     * cellFsSdataOpen, which ps3_compute_nid("cellFsSdataOpen") does NOT match
     * (the real exported symbol name differs from the friendly name). */
    ps3_hle_register_ctx(0xB1840B53u, "cellFsSdataOpen", cellFsSdataOpen);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsClose"),    "cellFsClose",    cellFsClose);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsRead"),     "cellFsRead",     cellFsRead);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsWrite"),    "cellFsWrite",    cellFsWrite);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsLseek"),    "cellFsLseek",    cellFsLseek);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsFGetBlockSize"), "cellFsFGetBlockSize", cellFsFGetBlockSize);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsGetBlockSize"),  "cellFsGetBlockSize",  cellFsGetBlockSize);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsStat"),     "cellFsStat",     cellFsStat);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsFstat"),    "cellFsFstat",    cellFsFstat);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsOpendir"),  "cellFsOpendir",  cellFsOpendir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsReaddir"),  "cellFsReaddir",  cellFsReaddir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsClosedir"), "cellFsClosedir", cellFsClosedir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsMkdir"),    "cellFsMkdir",    cellFsMkdir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsRmdir"),    "cellFsRmdir",    cellFsRmdir);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsUnlink"),   "cellFsUnlink",   cellFsUnlink);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsFsync"),    "cellFsFsync",    cellFsFsync);
    ps3_hle_register_ctx(ps3_compute_nid("cellFsAllocateFileAreaWithoutZeroFill"),
                         "cellFsAllocateFileAreaWithoutZeroFill", cellFsAllocateFileAreaWithoutZeroFill);
    /* AIO by literal import NID -- ps3_compute_nid() of the friendly name does
     * not match the exported symbols. */
    ps3_hle_register_ctx(0xDB869F20u, "cellFsAioInit",   cellFsAioInit);
    ps3_hle_register_ctx(0x9F951810u, "cellFsAioFinish", cellFsAioFinish);
    ps3_hle_register_ctx(0xC1C507E7u, "cellFsAioRead",   cellFsAioRead);
    ps3_hle_register_ctx(0x7F13FC8Cu, "cellFsAioCancel", cellFsAioCancel);
}
