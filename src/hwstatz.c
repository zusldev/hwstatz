/* hwstatz: 1 Hz click-through overlay.
   Tctl is SMN 0x59800. Package watts are MSR 0xC0010299 and 0xC001029B.
   Both go through the official PawnIO driver and the signed AMDFamily17
   module next to this exe. Only read calls are used. */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <winioctl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
#endif
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif
#define NVML_SUCCESS 0
#define NVML_TEMPERATURE_GPU 0
#define NVML_CLOCK_GRAPHICS 0
#define NVML_TOTAL_POWER_SAMPLES 0
#define NVML_VALUE_TYPE_DOUBLE 0
#define NVML_VALUE_TYPE_UNSIGNED_INT 1
#define NVML_VALUE_TYPE_UNSIGNED_LONG 2
#define NVML_VALUE_TYPE_UNSIGNED_LONG_LONG 3
#define NVML_VALUE_TYPE_SIGNED_LONG_LONG 4

#define HUD_MAX 12
#define DISK_MAX 8
#define GPU_MAX 4

/* 1 default: temperature and usage, no watts.
   2 simple: temperatures only.
   3 detailed: temperature, usage bar, watts, limit, RAM fraction, disk peak. */
static int cfg_preset = 1;
static int cfg_f = 0;

typedef void *nvmlDevice_t;
typedef int nvmlReturn_t;

typedef struct {
    unsigned int gpu;
    unsigned int memory;
} nvmlUtilization_t;

typedef union {
    double dVal;
    unsigned int uiVal;
    unsigned long ulVal;
    unsigned long long ullVal;
    long long sllVal;
} nvmlValue_t;

typedef struct {
    unsigned long long timeStamp;
    nvmlValue_t sampleValue;
} nvmlSample_t;

typedef struct {
    unsigned long long idle;
    unsigned long long kern;
    unsigned long long user;
} CpuTimes;

typedef struct {
    char name[40];
    int c;
    int peak;
} DiskRow;

typedef struct {
    int have_t;
    unsigned temp_c;
    int have_u;
    unsigned util;
    int have_w;
    double watts;
    int have_clock;
    unsigned clock;
    int have_limit;
    unsigned limit_w;
} GpuRow;

static unsigned long long ft_ull(const FILETIME *f) {
    return ((unsigned long long)f->dwHighDateTime << 32) | f->dwLowDateTime;
}

static int cpu_times(CpuTimes *t) {
    FILETIME idle, kern, user;
    if (!GetSystemTimes(&idle, &kern, &user)) return 0;
    t->idle = ft_ull(&idle);
    t->kern = ft_ull(&kern);
    t->user = ft_ull(&user);
    return 1;
}

static double cpu_load(const CpuTimes *a, const CpuTimes *b) {
    unsigned long long idle = b->idle - a->idle;
    unsigned long long kern = b->kern - a->kern;
    unsigned long long user = b->user - a->user;
    unsigned long long total = kern + user;
    if (total == 0 || kern < idle) return 0.0;
    return 100.0 * (double)(total - idle) / (double)total;
}

static int clamp_load(double v) {
    if (v < 0.0) return 0;
    if (v > 100.0) return 100;
    return (int)(v + 0.5);
}

static void trim_copy(char *dst, size_t dstn, const char *src) {
    while (*src == ' ') src++;
    size_t n = 0;
    while (src[n] && n + 1 < dstn) {
        dst[n] = src[n];
        n++;
    }
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\t')) n--;
    dst[n] = 0;
}

static void field_at(char *dst, size_t dstn, const BYTE *base, DWORD bytes, DWORD offset) {
    char tmp[64];
    size_t n = 0;
    dst[0] = 0;
    if (offset == 0 || offset >= bytes) return;
    while (offset + n < bytes && base[offset + n] && n + 1 < sizeof tmp) {
        tmp[n] = (char)base[offset + n];
        n++;
    }
    tmp[n] = 0;
    trim_copy(dst, dstn, tmp);
}

static int smart_temp(HANDLE h) {
    SENDCMDINPARAMS in;
    struct {
        SENDCMDOUTPARAMS hdr;
        BYTE extra[512];
    } out;
    DWORD got = 0;
    int i;
    memset(&in, 0, sizeof in);
    in.cBufferSize = READ_ATTRIBUTE_BUFFER_SIZE;
    in.irDriveRegs.bFeaturesReg = READ_ATTRIBUTES;
    in.irDriveRegs.bSectorCountReg = 1;
    in.irDriveRegs.bSectorNumberReg = 1;
    in.irDriveRegs.bCylLowReg = SMART_CYL_LOW;
    in.irDriveRegs.bCylHighReg = SMART_CYL_HI;
    in.irDriveRegs.bDriveHeadReg = 0xA0;
    in.irDriveRegs.bCommandReg = SMART_CMD;
    memset(&out, 0, sizeof out);
    if (!DeviceIoControl(h, SMART_RCV_DRIVE_DATA, &in, sizeof in, &out, sizeof out, &got, NULL))
        return -1;
    if (out.hdr.DriverStatus.bDriverError != SMART_NO_ERROR) return -1;
    {
        int fallback = -1;
        for (i = 0; i < 30; i++) {
            const BYTE *a = out.hdr.bBuffer + 2 + (i * 12);
            int t = a[5];
            if (t <= 0 || t >= 125) continue;
            if (a[0] == 194) return t;
            if (a[0] == 190 && fallback < 0) fallback = t;
        }
        return fallback;
    }
}

static DiskRow g_disks[DISK_MAX];
static int g_disk_n;
static ULONGLONG disk_at;
static int disk_valid;
static wchar_t hud_lines[HUD_MAX][160];
static int hud_bar[HUD_MAX];
static int hud_n;
static char pio_why[160];

static void hud_add(int bar, const wchar_t *fmt, ...) {
    va_list ap;
    if (hud_n >= HUD_MAX) return;
    va_start(ap, fmt);
    _vsnwprintf(hud_lines[hud_n], 159, fmt, ap);
    va_end(ap);
    hud_lines[hud_n][159] = 0;
    hud_bar[hud_n] = bar;
    hud_n++;
}

static void fmt_temp(wchar_t *dst, size_t n, double celsius, int decimals) {
    double v = cfg_f ? (celsius * 9.0 / 5.0 + 32.0) : celsius;
    size_t len;
    if (n == 0) return;
    dst[0] = 0;
    if (decimals) _snwprintf(dst, n, L"%.1f", v);
    else _snwprintf(dst, n, L"%.0f", v);
    dst[n - 1] = 0;
    len = wcslen(dst);
    if (len + 2 < n) {
        dst[len] = (wchar_t)0x00B0;
        dst[len + 1] = cfg_f ? L'F' : L'C';
        dst[len + 2] = 0;
    }
}

static void read_disks(void) {
    char path[32];
    int i;
    ULONGLONG now = GetTickCount64();
    /* SMART and the temperature ioctl can stall a spinning disk. */
    if (disk_valid && now - disk_at < 30000ull) return;
    g_disk_n = 0;
    for (i = 0; i < 16 && g_disk_n < DISK_MAX; i++) {
        HANDLE h;
        STORAGE_PROPERTY_QUERY q;
        BYTE desc_buf[1024];
        BYTE temp_buf[512];
        DWORD got = 0;
        STORAGE_DEVICE_DESCRIPTOR *desc;
        STORAGE_TEMPERATURE_DATA_DESCRIPTOR *temp;
        char vendor[40], product[40], name[80];
        int t, primary = 0, hottest = -127, reported = 0, have_primary = 0;
        WORD k;

        snprintf(path, sizeof path, "\\\\.\\PhysicalDrive%d", i);
        /* GENERIC_WRITE is the open mask SMART requires. The ioctl is a read. */
        h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;

        memset(&q, 0, sizeof q);
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, desc_buf, sizeof desc_buf, &got, NULL)) {
            CloseHandle(h);
            continue;
        }
        desc = (STORAGE_DEVICE_DESCRIPTOR *)desc_buf;
        field_at(vendor, sizeof vendor, desc_buf, got, desc->VendorIdOffset);
        field_at(product, sizeof product, desc_buf, got, desc->ProductIdOffset);
        if (vendor[0] && product[0]) snprintf(name, sizeof name, "%s %s", vendor, product);
        else if (product[0]) snprintf(name, sizeof name, "%s", product);
        else snprintf(name, sizeof name, "PhysicalDrive%d", i);

        memset(&q, 0, sizeof q);
        q.PropertyId = StorageDeviceTemperatureProperty;
        q.QueryType = PropertyStandardQuery;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, temp_buf, sizeof temp_buf, &got, NULL)) {
            temp = (STORAGE_TEMPERATURE_DATA_DESCRIPTOR *)temp_buf;
            if (temp->InfoCount > 0 && temp->InfoCount < 16) {
                for (k = 0; k < temp->InfoCount; k++) {
                    t = temp->TemperatureInfo[k].Temperature;
                    if ((unsigned short)t == STORAGE_TEMPERATURE_VALUE_NOT_REPORTED) continue;
                    if (temp->TemperatureInfo[k].Index == 0) {
                        primary = t;
                        have_primary = 1;
                    }
                    if (t > hottest) hottest = t;
                    reported = 1;
                }
            }
        }
        if (!reported) {
            t = smart_temp(h);
            if (t > 0) {
                primary = t;
                hottest = t;
                reported = 1;
            }
        }
        CloseHandle(h);
        if (!reported) continue;
        if (!have_primary) primary = hottest;
        if (strlen(name) > 32) name[32] = 0;
        memset(&g_disks[g_disk_n], 0, sizeof g_disks[0]);
        memcpy(g_disks[g_disk_n].name, name, strlen(name) + 1);
        g_disks[g_disk_n].c = primary;
        g_disks[g_disk_n].peak = hottest;
        g_disk_n++;
    }
    disk_at = GetTickCount64();
    disk_valid = 1;
}

static int sample_watts(const nvmlSample_t *s, unsigned int value_type, double *watts) {
    double v;
    switch (value_type) {
    case NVML_VALUE_TYPE_DOUBLE: v = s->sampleValue.dVal; break;
    case NVML_VALUE_TYPE_UNSIGNED_INT: v = (double)s->sampleValue.uiVal; break;
    case NVML_VALUE_TYPE_UNSIGNED_LONG: v = (double)s->sampleValue.ulVal; break;
    case NVML_VALUE_TYPE_UNSIGNED_LONG_LONG: v = (double)s->sampleValue.ullVal; break;
    case NVML_VALUE_TYPE_SIGNED_LONG_LONG: v = (double)s->sampleValue.sllVal; break;
    default: return 0;
    }
    if (v <= 0.0) return 0;
    /* NVML power samples are milliwatts. A double already in watts stays. */
    if (value_type != NVML_VALUE_TYPE_DOUBLE || v > 2000.0) v /= 1000.0;
    if (v <= 0.0 || v > 800.0) return 0;
    *watts = v;
    return 1;
}

static HMODULE nv;
static nvmlReturn_t (WINAPI *get_count)(unsigned int *);
static nvmlReturn_t (WINAPI *get_handle)(unsigned int, nvmlDevice_t *);
static nvmlReturn_t (WINAPI *get_temp)(nvmlDevice_t, int, unsigned int *);
static nvmlReturn_t (WINAPI *get_util)(nvmlDevice_t, nvmlUtilization_t *);
static nvmlReturn_t (WINAPI *get_clock)(nvmlDevice_t, int, unsigned int *);
static nvmlReturn_t (WINAPI *get_power)(nvmlDevice_t, unsigned int *);
static nvmlReturn_t (WINAPI *get_limit)(nvmlDevice_t, unsigned int *);
static nvmlReturn_t (WINAPI *get_samples)(nvmlDevice_t, int, unsigned long long, unsigned int *, unsigned int *, nvmlSample_t *);
static int nvml_ready;
static GpuRow g_gpus[GPU_MAX];
static int g_gpu_n;
static int g_gpu_missing;
static int g_ram_ok;
static unsigned long g_ram_pct;
static double g_ram_used;
static double g_ram_total;

static int nvml_init(void) {
    nvmlReturn_t (WINAPI *init)(void);
    if (nvml_ready) return nvml_ready > 0;
    nvml_ready = -1;
    nv = LoadLibraryExA("nvml.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!nv) return 0;
    init = (void *)GetProcAddress(nv, "nvmlInit_v2");
    if (!init) init = (void *)GetProcAddress(nv, "nvmlInit");
    get_count = (void *)GetProcAddress(nv, "nvmlDeviceGetCount");
    get_handle = (void *)GetProcAddress(nv, "nvmlDeviceGetHandleByIndex");
    get_temp = (void *)GetProcAddress(nv, "nvmlDeviceGetTemperature");
    get_util = (void *)GetProcAddress(nv, "nvmlDeviceGetUtilizationRates");
    get_clock = (void *)GetProcAddress(nv, "nvmlDeviceGetClockInfo");
    get_power = (void *)GetProcAddress(nv, "nvmlDeviceGetPowerUsage");
    get_limit = (void *)GetProcAddress(nv, "nvmlDeviceGetPowerManagementLimit");
    get_samples = (void *)GetProcAddress(nv, "nvmlDeviceGetSamples");
    if (!init || !get_count || !get_handle || init() != NVML_SUCCESS) return 0;
    nvml_ready = 1;
    return 1;
}

static void read_gpu(void) {
    unsigned int count = 0, i;
    g_gpu_n = 0;
    g_gpu_missing = 0;
    if (!nvml_init() || get_count(&count) != NVML_SUCCESS || count == 0) {
        g_gpu_missing = 1;
        return;
    }
    for (i = 0; i < count && g_gpu_n < GPU_MAX; i++) {
        nvmlDevice_t dev;
        unsigned int temp = 0, clock = 0, mw = 0, limit = 0;
        nvmlUtilization_t util;
        double watts = 0.0;
        int have_temp = 0, have_util = 0, have_w = 0, have_limit = 0, have_clock = 0;
        unsigned int s;
        unsigned long long newest = 0;
        GpuRow *row;

        if (get_handle(i, &dev) != NVML_SUCCESS) continue;
        if (get_temp && get_temp(dev, NVML_TEMPERATURE_GPU, &temp) == NVML_SUCCESS) have_temp = 1;
        memset(&util, 0, sizeof util);
        if (get_util && get_util(dev, &util) == NVML_SUCCESS) have_util = 1;
        if (get_clock && get_clock(dev, NVML_CLOCK_GRAPHICS, &clock) == NVML_SUCCESS && clock > 0)
            have_clock = 1;
        if (get_power && get_power(dev, &mw) == NVML_SUCCESS && mw > 0) {
            watts = mw / 1000.0;
            have_w = 1;
        } else if (get_samples) {
            nvmlSample_t samples[128];
            unsigned int value_type = 0, sample_count = 128;
            if (get_samples(dev, NVML_TOTAL_POWER_SAMPLES, 0, &value_type, &sample_count, samples) == NVML_SUCCESS) {
                for (s = 0; s < sample_count && s < 128; s++) {
                    double w;
                    if (samples[s].timeStamp < newest) continue;
                    if (!sample_watts(&samples[s], value_type, &w)) continue;
                    newest = samples[s].timeStamp;
                    watts = w;
                    have_w = 1;
                }
            }
        }
        if (get_limit && get_limit(dev, &limit) == NVML_SUCCESS && limit > 0) have_limit = 1;
        row = &g_gpus[g_gpu_n];
        memset(row, 0, sizeof *row);
        row->have_t = have_temp;
        row->temp_c = temp;
        row->have_u = have_util;
        row->util = util.gpu;
        row->have_w = have_w;
        row->watts = watts;
        row->have_clock = have_clock;
        row->clock = clock;
        if (have_limit) {
            row->have_limit = 1;
            row->limit_w = limit >= 1000 ? (limit + 500) / 1000 : limit;
        }
        g_gpu_n++;
    }
    if (g_gpu_n == 0) g_gpu_missing = 1;
}

static void read_ram(void) {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof ms;
    g_ram_ok = 0;
    if (!GlobalMemoryStatusEx(&ms)) return;
    g_ram_ok = 1;
    g_ram_pct = ms.dwMemoryLoad;
    g_ram_total = (double)ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    g_ram_used = (double)(ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
}

#define PIO_DLL "C:\\Program Files\\PawnIO\\PawnIOLib.dll"
#define MSR_RAPL_PWR_UNIT 0xC0010299ull
#define MSR_PKG_ENERGY_STATUS 0xC001029Bull
#define SMN_THM_TCON_CUR_TMP 0x00059800ull

typedef long (__stdcall *pio_open_fn)(HANDLE *);
typedef long (__stdcall *pio_load_fn)(HANDLE, const unsigned char *, size_t);
typedef long (__stdcall *pio_exec_fn)(HANDLE, const char *, const unsigned long long *, size_t,
                                      unsigned long long *, size_t, size_t *);
typedef long (__stdcall *pio_close_fn)(HANDLE);

static HMODULE pio_mod;
static HANDLE pio_dev;
static pio_exec_fn pio_exec;
static pio_close_fn pio_close;
static int pio_ready;
static int pio_warned;
static int energy_shift = -1;
static unsigned long long energy_last;
static LARGE_INTEGER energy_qpc;
static int energy_have;

static void pio_fail(const char *why) {
    if (pio_warned) return;
    snprintf(pio_why, sizeof pio_why, "%s", why);
    pio_warned = 1;
}

static int load_module_blob(unsigned char **out, size_t *outn) {
    char path[MAX_PATH];
    char *slash;
    HANDLE f;
    DWORD sz, got = 0;
    unsigned char *buf;
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    slash = strrchr(path, '\\');
    if (!slash) return 0;
    if ((size_t)(slash - path) + 1 + sizeof "AMDFamily17.bin" > MAX_PATH) return 0;
    memcpy(slash + 1, "AMDFamily17.bin", sizeof "AMDFamily17.bin");
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    sz = GetFileSize(f, NULL);
    if (sz < 64 || sz > 1024u * 1024u) {
        CloseHandle(f);
        return 0;
    }
    buf = (unsigned char *)malloc(sz);
    if (!buf) {
        CloseHandle(f);
        return 0;
    }
    if (!ReadFile(f, buf, sz, &got, NULL) || got != sz) {
        free(buf);
        CloseHandle(f);
        return 0;
    }
    CloseHandle(f);
    *out = buf;
    *outn = (size_t)sz;
    return 1;
}

static int pio_init(void) {
    pio_open_fn open_fn;
    pio_load_fn load_fn;
    unsigned char *blob = NULL;
    size_t blen = 0;
    long hr;
    if (pio_ready > 0) return 1;
    if (!pio_mod)
        pio_mod = LoadLibraryExA(PIO_DLL, NULL,
            LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!pio_mod) {
        pio_fail("falta PawnIOLib.dll");
        return 0;
    }
    open_fn = (pio_open_fn)(void *)GetProcAddress(pio_mod, "pawnio_open");
    load_fn = (pio_load_fn)(void *)GetProcAddress(pio_mod, "pawnio_load");
    pio_exec = (pio_exec_fn)(void *)GetProcAddress(pio_mod, "pawnio_execute");
    pio_close = (pio_close_fn)(void *)GetProcAddress(pio_mod, "pawnio_close");
    if (!open_fn || !load_fn || !pio_exec || !pio_close) {
        pio_fail("PawnIOLib no exporta la API de lectura");
        return 0;
    }
    hr = open_fn(&pio_dev);
    if (hr < 0 || !pio_dev) {
        pio_fail("no se abrio PawnIO; el servicio tiene que estar en ejecucion y el proceso, ser administrador");
        return 0;
    }
    if (!load_module_blob(&blob, &blen)) {
        pio_close(pio_dev);
        pio_dev = NULL;
        pio_fail("falta AMDFamily17.bin junto a hwstatz.exe");
        return 0;
    }
    hr = load_fn(pio_dev, blob, blen);
    free(blob);
    if (hr < 0) {
        pio_close(pio_dev);
        pio_dev = NULL;
        pio_fail("el driver rechazo el modulo firmado");
        return 0;
    }
    pio_ready = 1;
    return 1;
}

static int pio_call(const char *name, unsigned long long in, unsigned long long *out) {
    unsigned long long tmp = 0;
    size_t ret = 0;
    long hr;
    if (strcmp(name, "ioctl_read_msr") != 0 && strcmp(name, "ioctl_read_smn") != 0) return 0;
    if (!pio_init()) return 0;
    hr = pio_exec(pio_dev, name, &in, 1, &tmp, 1, &ret);
    if (hr < 0 || ret < 1) return 0;
    *out = tmp;
    return 1;
}

static int read_tctl(double *celsius) {
    HANDLE mx;
    DWORD wait;
    unsigned long long reg = 0;
    int mc;
    int ok;
    mx = CreateMutexW(NULL, FALSE, L"Global\\Access_PCI");
    if (!mx) return 0;
    wait = WaitForSingleObject(mx, 200);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mx);
        return 0;
    }
    ok = pio_call("ioctl_read_smn", SMN_THM_TCON_CUR_TMP, &reg);
    ReleaseMutex(mx);
    CloseHandle(mx);
    if (!ok) return 0;
    reg &= 0xffffffffull;
    mc = (int)((reg >> 21) * 125u);
    if (reg & (1ull << 19)) mc -= 49000;
    if (mc < -10000 || mc > 125000) return 0;
    *celsius = (double)mc / 1000.0;
    return 1;
}

static int energy_unit(void) {
    unsigned long long raw = 0;
    unsigned shift;
    if (energy_shift >= 0) return energy_shift;
    if (!pio_call("ioctl_read_msr", MSR_RAPL_PWR_UNIT, &raw)) return -1;
    shift = (unsigned)((raw >> 8) & 0x1fu);
    if (shift == 0 || shift > 20) return -1;
    energy_shift = (int)shift;
    return energy_shift;
}

/* Stores the package-energy counter. The next package_watts() call is the average since this mark. */
static void cpu_mark(void) {
    unsigned long long raw = 0;
    if (energy_unit() < 0) return;
    if (!pio_call("ioctl_read_msr", MSR_PKG_ENERGY_STATUS, &raw)) return;
    energy_last = raw & 0xffffffffull;
    QueryPerformanceCounter(&energy_qpc);
    energy_have = 1;
}

static int package_watts(double *watts) {
    unsigned long long raw = 0, delta;
    LARGE_INTEGER now, freq;
    double seconds, joules, w;
    int shift;
    if (!energy_have) {
        cpu_mark();
        return 0;
    }
    shift = energy_unit();
    if (shift < 0) return 0;
    if (!pio_call("ioctl_read_msr", MSR_PKG_ENERGY_STATUS, &raw)) return 0;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart == 0) return 0;
    raw &= 0xffffffffull;
    delta = (raw - energy_last) & 0xffffffffull;
    seconds = (double)(now.QuadPart - energy_qpc.QuadPart) / (double)freq.QuadPart;
    energy_last = raw;
    energy_qpc = now;
    if (seconds < 0.05) return 0;
    joules = (double)delta / (double)(1u << shift);
    w = joules / seconds;
    if (w < 0.0 || w > 200.0) return 0;
    *watts = w;
    return 1;
}

static void render_cpu(const CpuTimes *a, const CpuTimes *b) {
    double tctl = 0.0, watts = 0.0, load;
    int have_t, have_w;
    wchar_t tbuf[32], wbuf[32];
    have_t = read_tctl(&tctl);
    have_w = package_watts(&watts);
    load = cpu_load(a, b);
    if (have_t) fmt_temp(tbuf, 32, tctl, cfg_preset == 2 ? 0 : 1);
    else wcscpy(tbuf, L"n/d");
    if (cfg_preset == 2) {
        hud_add(-1, L"CPU  %s", tbuf);
    } else if (cfg_preset == 1) {
        hud_add(-1, L"CPU  %s    %3.0f%%", tbuf, load);
    } else {
        if (have_w) _snwprintf(wbuf, 32, L"%3.0f W", watts);
        else wcscpy(wbuf, L"n/d");
        wbuf[31] = 0;
        hud_add(clamp_load(load), L"CPU  %s    %s    %3.0f%%", tbuf, wbuf, load);
    }
    if (!have_t && pio_why[0]) {
        wchar_t why[160];
        MultiByteToWideChar(CP_ACP, 0, pio_why, -1, why, 160);
        hud_add(-1, L"%s", why);
    }
}

static void render_ram(void) {
    if (cfg_preset == 2) return;
    if (!g_ram_ok) {
        hud_add(-1, L"RAM  n/d");
        return;
    }
    if (cfg_preset == 1)
        hud_add(-1, L"RAM  %3lu%%", g_ram_pct);
    else
        hud_add((int)(g_ram_pct > 100 ? 100 : g_ram_pct),
                L"RAM  %3lu%%    %.1f / %.1f GB", g_ram_pct, g_ram_used, g_ram_total);
}

static void render_gpu(void) {
    int i;
    if (g_gpu_missing || g_gpu_n == 0) {
        hud_add(-1, L"GPU  no disponible");
        return;
    }
    for (i = 0; i < g_gpu_n; i++) {
        GpuRow *g = &g_gpus[i];
        wchar_t tbuf[32], wbuf[32];
        int decimals = (cfg_preset == 2 || !cfg_f) ? 0 : 1;
        if (g->have_t) fmt_temp(tbuf, 32, (double)g->temp_c, decimals);
        else wcscpy(tbuf, L"n/d");
        if (cfg_preset == 2) {
            hud_add(-1, L"GPU  %s", tbuf);
            continue;
        }
        if (cfg_preset == 1) {
            if (g->have_u) hud_add(-1, L"GPU  %s    %3u%%", tbuf, g->util);
            else hud_add(-1, L"GPU  %s", tbuf);
            continue;
        }
        if (g->have_w) _snwprintf(wbuf, 32, L"%3.0f W", g->watts);
        else wcscpy(wbuf, L"n/d");
        wbuf[31] = 0;
        if (g->have_u)
            hud_add(g->util > 100 ? 100 : (int)g->util, L"GPU  %s    %s    %3u%%", tbuf, wbuf, g->util);
        else
            hud_add(-1, L"GPU  %s    %s", tbuf, wbuf);
        if (g->have_clock && g->have_limit)
            hud_add(-1, L"     %u MHz    l\u00EDmite %u W", g->clock, g->limit_w);
        else if (g->have_clock)
            hud_add(-1, L"     %u MHz", g->clock);
        else if (g->have_limit)
            hud_add(-1, L"     l\u00EDmite %u W", g->limit_w);
    }
}

static void render_disk(const DiskRow *d) {
    wchar_t name[40], tbuf[32], pbuf[32];
    int decimals = cfg_f ? 1 : 0;
    if (cfg_preset == 2) decimals = 0;
    MultiByteToWideChar(CP_ACP, 0, d->name, -1, name, 40);
    fmt_temp(tbuf, 32, (double)d->c, decimals);
    if (cfg_preset == 3 && d->peak > d->c) {
        fmt_temp(pbuf, 32, (double)d->peak, decimals);
        hud_add(-1, L"%s  %s    pico %s", name, tbuf, pbuf);
    } else {
        hud_add(-1, L"%s  %s", name, tbuf);
    }
}

static void report(const CpuTimes *a, const CpuTimes *b) {
    int i;
    hud_n = 0;
    render_cpu(a, b);
    read_ram();
    render_ram();
    read_gpu();
    render_gpu();
    read_disks();
    for (i = 0; i < g_disk_n; i++) render_disk(&g_disks[i]);
}

#define WM_TRAY (WM_APP + 1)
#define WM_APP_SHOW (WM_APP + 2)
#define ID_TOGGLE 1001
#define ID_EXIT 1002
#define ID_MON_BASE 1100
#define ID_CORNER_BASE 1200
#define ID_PRESET_DEFAULT 1301
#define ID_PRESET_SIMPLE 1302
#define ID_PRESET_DETAILED 1303
#define ID_UNIT_C 1401
#define ID_UNIT_F 1402
#define TIMER_SAMPLE 1

static HWND g_hwnd;
static HANDLE g_mutex;
static HICON g_icon;
static HFONT g_font;
static NOTIFYICONDATAW g_nid;
static int g_tray;
static int g_tray_v4;
static UINT g_dpi = 96;
static int cfg_mon = 1;
static int cfg_corner = 3;
static int cfg_visible = 1;
static int g_locked_w;
static int g_lock_key = -1;
static wchar_t cfg_path[MAX_PATH];
static CpuTimes g_prev;
static int g_have_times;

typedef struct {
    RECT work;
    int index;
    wchar_t name[32];
} MonInfo;

static MonInfo g_mons[8];
static int g_mon_n;

static void stop_exe(const wchar_t *exe) {
    HANDLE snap;
    PROCESSENTRY32W pe;
    DWORD self = GetCurrentProcessId();
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) {
        do {
            HANDLE proc;
            if (pe.th32ProcessID == self) continue;
            if (_wcsicmp(pe.szExeFile, exe) != 0) continue;
            proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
            if (!proc) continue;
            TerminateProcess(proc, 0);
            WaitForSingleObject(proc, 2000);
            CloseHandle(proc);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

/* The previous overlay used this class. An unrelated hwstat.exe is left alone. */
static void stop_old_overlay(void) {
    HWND w;
    DWORD pid = 0;
    HANDLE proc;
    w = FindWindowW(L"hwstat_overlay", NULL);
    if (!w) return;
    GetWindowThreadProcessId(w, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return;
    proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!proc) return;
    TerminateProcess(proc, 0);
    WaitForSingleObject(proc, 2000);
    CloseHandle(proc);
}

static int take_instance(void) {
    HWND other;
    DWORD_PTR ping = 0;
    g_mutex = CreateMutexW(NULL, TRUE, L"Local\\hwstatz.overlay");
    if (!g_mutex) return 0;
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        stop_old_overlay();
        return 1;
    }
    other = FindWindowW(L"hwstatz_overlay", NULL);
    if (other && SendMessageTimeoutW(other, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 800, &ping)) {
        PostMessageW(other, WM_APP_SHOW, 0, 0);
        CloseHandle(g_mutex);
        g_mutex = NULL;
        return 0;
    }
    stop_exe(L"hwstatz.exe");
    stop_old_overlay();
    CloseHandle(g_mutex);
    Sleep(300);
    g_mutex = CreateMutexW(NULL, TRUE, L"Local\\hwstatz.overlay");
    if (!g_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_mutex) CloseHandle(g_mutex);
        g_mutex = NULL;
        return 0;
    }
    return 1;
}

static void config_path(void) {
    wchar_t *slash;
    DWORD n = GetModuleFileNameW(NULL, cfg_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        cfg_path[0] = 0;
        return;
    }
    slash = wcsrchr(cfg_path, L'\\');
    if (!slash) return;
    wcscpy(slash + 1, L"hwstatz.ini");
}

static void config_load(void) {
    wchar_t unit[8];
    if (!cfg_path[0]) return;
    cfg_mon = GetPrivateProfileIntW(L"hwstatz", L"monitor", 1, cfg_path);
    cfg_corner = GetPrivateProfileIntW(L"hwstatz", L"corner", 3, cfg_path);
    cfg_visible = GetPrivateProfileIntW(L"hwstatz", L"visible", 1, cfg_path) ? 1 : 0;
    cfg_preset = GetPrivateProfileIntW(L"hwstatz", L"preset", 1, cfg_path);
    GetPrivateProfileStringW(L"hwstatz", L"unit", L"C", unit, 8, cfg_path);
    cfg_f = (unit[0] == L'F' || unit[0] == L'f') ? 1 : 0;
    if (cfg_mon < 1) cfg_mon = 1;
    if (cfg_corner < 0 || cfg_corner > 3) cfg_corner = 3;
    if (cfg_preset < 1 || cfg_preset > 3) cfg_preset = 1;
}

static void config_save(void) {
    wchar_t buf[16];
    if (!cfg_path[0]) return;
    _snwprintf(buf, 16, L"%d", cfg_mon);
    WritePrivateProfileStringW(L"hwstatz", L"monitor", buf, cfg_path);
    _snwprintf(buf, 16, L"%d", cfg_corner);
    WritePrivateProfileStringW(L"hwstatz", L"corner", buf, cfg_path);
    WritePrivateProfileStringW(L"hwstatz", L"visible", cfg_visible ? L"1" : L"0", cfg_path);
    _snwprintf(buf, 16, L"%d", cfg_preset);
    WritePrivateProfileStringW(L"hwstatz", L"preset", buf, cfg_path);
    WritePrivateProfileStringW(L"hwstatz", L"unit", cfg_f ? L"F" : L"C", cfg_path);
}

static BOOL CALLBACK mon_enum(HMONITOR monitor, HDC dc, LPRECT rc, LPARAM lp) {
    MONITORINFOEXW mi;
    int number;
    (void)dc;
    (void)rc;
    (void)lp;
    if (g_mon_n >= 8) return TRUE;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(monitor, (LPMONITORINFO)&mi)) return TRUE;
    number = g_mon_n + 1;
    if (wcsncmp(mi.szDevice, L"\\\\.\\DISPLAY", 11) == 0) number = _wtoi(mi.szDevice + 11);
    if (number < 1) number = g_mon_n + 1;
    g_mons[g_mon_n].work = mi.rcWork;
    g_mons[g_mon_n].index = number;
    _snwprintf(g_mons[g_mon_n].name, 32, L"Pantalla %d", number);
    g_mon_n++;
    return TRUE;
}

static void refresh_monitors(void) {
    int i, j;
    g_mon_n = 0;
    EnumDisplayMonitors(NULL, NULL, mon_enum, 0);
    for (i = 0; i < g_mon_n; i++) {
        for (j = i + 1; j < g_mon_n; j++) {
            if (g_mons[j].index < g_mons[i].index) {
                MonInfo tmp = g_mons[i];
                g_mons[i] = g_mons[j];
                g_mons[j] = tmp;
            }
        }
    }
}

static int monitor_slot(void) {
    int i;
    for (i = 0; i < g_mon_n; i++) {
        if (g_mons[i].index == cfg_mon) return i;
    }
    return 0;
}

static void read_dpi(HWND hwnd) {
    HMODULE shcore;
    typedef HRESULT (WINAPI *dpi_fn)(HMONITOR, int, UINT *, UINT *);
    dpi_fn get_dpi;
    HMONITOR monitor;
    UINT x = 96, y = 96;
    shcore = LoadLibraryExA("shcore.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!shcore) return;
    get_dpi = (dpi_fn)(void *)GetProcAddress(shcore, "GetDpiForMonitor");
    monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (get_dpi && monitor && get_dpi(monitor, 0, &x, &y) == S_OK && x >= 96) g_dpi = x;
    FreeLibrary(shcore);
}

static void make_font(void) {
    if (g_font) DeleteObject(g_font);
    g_font = CreateFontW(-MulDiv(14, (int)g_dpi, 96), 0, 0, 0, FW_NORMAL,
                         FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FF_DONTCARE, L"Segoe UI");
}

static HICON make_icon(void) {
    BITMAPINFO bi;
    HDC screen, mem;
    void *bits = NULL;
    HBITMAP color, mask;
    ICONINFO info;
    HICON icon;
    DWORD *px;
    int x, y;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 32;
    bi.bmiHeader.biHeight = -32;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    screen = GetDC(NULL);
    color = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    mem = CreateCompatibleDC(screen);
    SelectObject(mem, color);
    px = (DWORD *)bits;
    if (px) {
        for (y = 0; y < 32; y++) {
            for (x = 0; x < 32; x++) {
                int edge = (x < 2 || y < 2 || x > 29 || y > 29);
                px[y * 32 + x] = edge ? 0xFF3A342Cu : 0xFF141210u;
            }
        }
        for (y = 20; y < 24; y++) {
            for (x = 7; x < 25; x++) px[y * 32 + x] = 0xFFE7E1D6u;
        }
    }
    mask = CreateBitmap(32, 32, 1, 1, NULL);
    memset(&info, 0, sizeof info);
    info.fIcon = TRUE;
    info.hbmMask = mask;
    info.hbmColor = color;
    icon = CreateIconIndirect(&info);
    DeleteDC(mem);
    DeleteObject(color);
    DeleteObject(mask);
    ReleaseDC(NULL, screen);
    return icon;
}

static void tray_add(HWND hwnd) {
    memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icon;
    wcscpy(g_nid.szTip, L"hwstatz");
    g_tray = Shell_NotifyIconW(NIM_ADD, &g_nid) ? 1 : 0;
    g_tray_v4 = 0;
    if (g_tray) {
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        g_tray_v4 = Shell_NotifyIconW(NIM_SETVERSION, &g_nid) ? 1 : 0;
    }
}

static void tray_del(void) {
    if (!g_tray) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_tray = 0;
}

static void sample_now(void) {
    CpuTimes now;
    if (!cpu_times(&now)) return;
    if (!g_have_times) {
        g_prev = now;
        g_have_times = 1;
        cpu_mark();
    }
    report(&g_prev, &now);
    g_prev = now;
}

static int bar_px(void) { return MulDiv(72, (int)g_dpi, 96); }
static int bar_gap(void) { return MulDiv(12, (int)g_dpi, 96); }

static void place_window(HWND hwnd) {
    HDC dc;
    HFONT old;
    int i, max_w = 0, extra = 0, pad, line_h, w, h, margin, x, y, slot, key;
    UINT dpi_before;
    refresh_monitors();
    if (g_mon_n <= 0) return;
    dpi_before = g_dpi;
    read_dpi(hwnd);
    if (g_dpi != dpi_before || !g_font) make_font();
    slot = monitor_slot();
    pad = MulDiv(12, (int)g_dpi, 96);
    line_h = MulDiv(20, (int)g_dpi, 96);
    margin = MulDiv(16, (int)g_dpi, 96);
    dc = GetDC(hwnd);
    old = (HFONT)SelectObject(dc, g_font);
    for (i = 0; i < hud_n; i++) {
        RECT tr;
        SetRectEmpty(&tr);
        DrawTextW(dc, hud_lines[i], -1, &tr, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        if (tr.right > max_w) max_w = tr.right;
        if (hud_bar[i] >= 0) extra = bar_px() + bar_gap();
    }
    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
    if (hud_n == 0) max_w = MulDiv(120, (int)g_dpi, 96);
    if (max_w < MulDiv(148, (int)g_dpi, 96)) max_w = MulDiv(148, (int)g_dpi, 96);
    w = max_w + extra + pad * 2;
    key = cfg_preset + (cfg_f ? 10 : 0);
    if (key != g_lock_key) {
        g_locked_w = 0;
        g_lock_key = key;
    }
    if (w < g_locked_w) w = g_locked_w;
    else g_locked_w = w;
    h = (hud_n > 0 ? hud_n : 1) * line_h + pad * 2;
    if (cfg_corner == 0 || cfg_corner == 2) x = g_mons[slot].work.left + margin;
    else x = g_mons[slot].work.right - w - margin;
    if (cfg_corner == 0 || cfg_corner == 1) y = g_mons[slot].work.top + margin;
    else y = g_mons[slot].work.bottom - h - margin;
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | (cfg_visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    SetWindowRgn(hwnd, CreateRoundRectRgn(0, 0, w + 1, h + 1, MulDiv(8, (int)g_dpi, 96), MulDiv(8, (int)g_dpi, 96)), TRUE);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void paint_bar(HDC dc, RECT line, int pct) {
    RECT track, fill;
    int bw = bar_px();
    int bh = MulDiv(6, (int)g_dpi, 96);
    HBRUSH brush;
    if (pct < 0) return;
    if (pct > 100) pct = 100;
    track.right = line.right;
    track.left = track.right - bw;
    track.top = line.top + ((line.bottom - line.top) - bh) / 2;
    track.bottom = track.top + bh;
    brush = CreateSolidBrush(RGB(58, 52, 44));
    FillRect(dc, &track, brush);
    DeleteObject(brush);
    fill = track;
    fill.right = fill.left + (bw * pct) / 100;
    if (pct > 0 && fill.right == fill.left) fill.right = fill.left + 1;
    brush = CreateSolidBrush(RGB(231, 225, 214));
    FillRect(dc, &fill, brush);
    DeleteObject(brush);
}

static void paint_overlay(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc;
    RECT rc;
    HBRUSH bg;
    HPEN pen, old_pen;
    HGDIOBJ old_brush;
    HFONT old_font;
    int i, pad, line_h;
    dc = BeginPaint(hwnd, &ps);
    GetClientRect(hwnd, &rc);
    bg = CreateSolidBrush(RGB(20, 18, 16));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    pen = CreatePen(PS_SOLID, 1, RGB(58, 52, 44));
    old_pen = (HPEN)SelectObject(dc, pen);
    old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, 0, 0, rc.right, rc.bottom);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    old_font = (HFONT)SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(244, 241, 234));
    pad = MulDiv(12, (int)g_dpi, 96);
    line_h = MulDiv(20, (int)g_dpi, 96);
    for (i = 0; i < hud_n; i++) {
        RECT line, text;
        line.left = pad;
        line.right = rc.right - pad;
        line.top = pad + i * line_h;
        line.bottom = line.top + line_h;
        text = line;
        if (hud_bar[i] >= 0) {
            paint_bar(dc, line, hud_bar[i]);
            text.right -= bar_px() + bar_gap();
        }
        DrawTextW(dc, hud_lines[i], -1, &text, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(dc, old_font);
    EndPaint(hwnd, &ps);
}

static void show_menu(HWND hwnd) {
    HMENU menu;
    POINT pt;
    int i;
    refresh_monitors();
    menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TOGGLE, cfg_visible ? L"Ocultar" : L"Mostrar");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (cfg_preset == 1 ? MF_CHECKED : 0), ID_PRESET_DEFAULT, L"1  Default");
    AppendMenuW(menu, MF_STRING | (cfg_preset == 2 ? MF_CHECKED : 0), ID_PRESET_SIMPLE, L"2  Simple");
    AppendMenuW(menu, MF_STRING | (cfg_preset == 3 ? MF_CHECKED : 0), ID_PRESET_DETAILED, L"3  Detallado");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (cfg_f ? 0 : MF_CHECKED), ID_UNIT_C, L"Celsius");
    AppendMenuW(menu, MF_STRING | (cfg_f ? MF_CHECKED : 0), ID_UNIT_F, L"Fahrenheit");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < g_mon_n; i++) {
        UINT flags = MF_STRING;
        if (g_mons[i].index == cfg_mon) flags |= MF_CHECKED;
        AppendMenuW(menu, flags, ID_MON_BASE + g_mons[i].index, g_mons[i].name);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (cfg_corner == 0 ? MF_CHECKED : 0), ID_CORNER_BASE + 0, L"Arriba izquierda");
    AppendMenuW(menu, MF_STRING | (cfg_corner == 1 ? MF_CHECKED : 0), ID_CORNER_BASE + 1, L"Arriba derecha");
    AppendMenuW(menu, MF_STRING | (cfg_corner == 2 ? MF_CHECKED : 0), ID_CORNER_BASE + 2, L"Abajo izquierda");
    AppendMenuW(menu, MF_STRING | (cfg_corner == 3 ? MF_CHECKED : 0), ID_CORNER_BASE + 3, L"Abajo derecha");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Salir");
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static void apply_view(HWND hwnd) {
    config_save();
    if (!cfg_visible) return;
    sample_now();
    place_window(hwnd);
}

static void on_command(HWND hwnd, int id) {
    if (id == ID_TOGGLE) {
        cfg_visible = !cfg_visible;
        config_save();
        if (cfg_visible) {
            sample_now();
            place_window(hwnd);
            SetTimer(hwnd, TIMER_SAMPLE, 1000, NULL);
        } else {
            KillTimer(hwnd, TIMER_SAMPLE);
            ShowWindow(hwnd, SW_HIDE);
        }
        return;
    }
    if (id == ID_EXIT) {
        DestroyWindow(hwnd);
        return;
    }
    if (id == ID_PRESET_DEFAULT || id == ID_PRESET_SIMPLE || id == ID_PRESET_DETAILED) {
        cfg_preset = id - ID_PRESET_DEFAULT + 1;
        apply_view(hwnd);
        return;
    }
    if (id == ID_UNIT_C || id == ID_UNIT_F) {
        cfg_f = id == ID_UNIT_F;
        apply_view(hwnd);
        return;
    }
    if (id >= ID_MON_BASE && id < ID_MON_BASE + 16) {
        cfg_mon = id - ID_MON_BASE;
        config_save();
        if (cfg_visible) place_window(hwnd);
        return;
    }
    if (id >= ID_CORNER_BASE && id < ID_CORNER_BASE + 4) {
        cfg_corner = id - ID_CORNER_BASE;
        config_save();
        if (cfg_visible) place_window(hwnd);
    }
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    UINT ev;
    switch (msg) {
    case WM_TRAY:
        /* Version 4 puts the event in LOWORD(lParam). A left click is NIN_SELECT
           and a right click is WM_CONTEXTMENU. The button-up messages are the
           classic shell, and on version 4 they arrive for the same click. */
        ev = LOWORD(lp);
        if (g_tray_v4) {
            if (ev == NIN_SELECT || ev == NIN_KEYSELECT)
                on_command(hwnd, ID_TOGGLE);
            else if (ev == WM_CONTEXTMENU)
                show_menu(hwnd);
        } else if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK || ev == NIN_SELECT) {
            on_command(hwnd, ID_TOGGLE);
        } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
            show_menu(hwnd);
        }
        return 0;
    case WM_COMMAND:
        on_command(hwnd, LOWORD(wp));
        return 0;
    case WM_APP_SHOW:
        cfg_visible = 1;
        config_save();
        sample_now();
        place_window(hwnd);
        SetTimer(hwnd, TIMER_SAMPLE, 1000, NULL);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_SAMPLE && cfg_visible) {
            sample_now();
            place_window(hwnd);
        }
        return 0;
    case WM_DISPLAYCHANGE:
        if (cfg_visible) place_window(hwnd);
        return 0;
    case WM_DPICHANGED:
        g_dpi = HIWORD(wp);
        make_font();
        if (cfg_visible) place_window(hwnd);
        return 0;
    case WM_PAINT:
        paint_overlay(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_SAMPLE);
        tray_del();
        if (g_font) {
            DeleteObject(g_font);
            g_font = NULL;
        }
        if (g_icon) {
            DestroyIcon(g_icon);
            g_icon = NULL;
        }
        if (g_mutex) {
            ReleaseMutex(g_mutex);
            CloseHandle(g_mutex);
            g_mutex = NULL;
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    WNDCLASSW wc;
    MSG msg;
    (void)prev;
    (void)cmd;
    (void)show;
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    if (!take_instance()) return 0;
    config_path();
    config_load();
    g_icon = make_icon();
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"hwstatz_overlay";
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512));
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        L"hwstatz_overlay", L"hwstatz", WS_POPUP,
        0, 0, 220, 120, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;
    SetLayeredWindowAttributes(g_hwnd, 0, 232, LWA_ALPHA);
    make_font();
    tray_add(g_hwnd);
    /* The window exists before the slow reads, so a second launch can see it. */
    if (cpu_times(&g_prev)) {
        g_have_times = 1;
        cpu_mark();
    }
    if (cfg_visible) {
        sample_now();
        place_window(g_hwnd);
        SetTimer(g_hwnd, TIMER_SAMPLE, 1000, NULL);
    }
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
