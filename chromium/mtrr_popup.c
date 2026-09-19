/* Periodic MTRR dump via GamePP's HWiNFO device. C only; no extra link flags. */
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "libavutil/mem.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

enum { kIoctlRdMsr = 0x85FE2604u, kIntervalMs = 60000 };

#pragma pack(push, 1)
struct MsrBuf {
  uint32_t msr;
  uint32_t reserved;
  uint64_t value;
};
#pragma pack(pop)

_Static_assert(sizeof(struct MsrBuf) == 16, "RdMSR buffer must be 16 bytes");

static HANDLE g_stop;

static const char *MtrrTypeName(uint8_t t) {
  switch (t) {
    case 0: return "UC";
    case 1: return "WC";
    case 4: return "WT";
    case 5: return "WP";
    case 6: return "WB";
    default: return "?";
  }
}

static int WaitStop(DWORD ms) {
  return g_stop && WaitForSingleObject(g_stop, ms) == WAIT_OBJECT_0;
}

static void EnableLoadDriverPrivilege(void) {
  HANDLE token = NULL;
  TOKEN_PRIVILEGES tp;
  LUID luid;
  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
    return;
  }
  if (!LookupPrivilegeValueW(NULL, L"SeLoadDriverPrivilege", &luid)) {
    CloseHandle(token);
    return;
  }
  memset(&tp, 0, sizeof(tp));
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Luid = luid;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
  CloseHandle(token);
}

static HANDLE OpenHwinfoDevice(void) {
  static const char *names[] = {
      "\\\\.\\HWiNFO_216", "\\\\.\\HWiNFO_215", "\\\\.\\HWiNFO_217",
      "\\\\.\\HWiNFO_214", "\\\\.\\Global\\HWiNFO_216",
      "\\\\.\\Global\\HWiNFO_215",
  };
  unsigned i;
  for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    HANDLE h = CreateFileA(names[i], GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE)
      return h;
  }
  return INVALID_HANDLE_VALUE;
}

static int ReadMsr(HANDLE dev, uint32_t msr, uint64_t *out, DWORD *err_out) {
  struct MsrBuf buf;
  DWORD returned = 0;
  memset(&buf, 0, sizeof(buf));
  buf.msr = msr;
  if (!DeviceIoControl(dev, kIoctlRdMsr, &buf, sizeof(buf), &buf, sizeof(buf),
                       &returned, NULL)) {
    if (err_out)
      *err_out = GetLastError();
    return 0;
  }
  *out = buf.value;
  if (err_out)
    *err_out = 0;
  return 1;
}

static void AppendLine(char *dst, size_t cap, const char *line) {
  size_t used = strlen(dst);
  size_t n = strlen(line);
  if (used + n + 2 >= cap)
    return;
  memcpy(dst + used, line, n);
  dst[used + n] = '\r';
  dst[used + n + 1] = '\n';
  dst[used + n + 2] = 0;
}

static void DumpOne(HANDLE dev, char *text, size_t cap, uint32_t msr,
                    const char *label) {
  uint64_t v = 0;
  DWORD err = 0;
  char line[160];
  if (ReadMsr(dev, msr, &v, &err))
    snprintf(line, sizeof(line), "0x%03X %-22s %016llX", msr, label,
             (unsigned long long)v);
  else
    snprintf(line, sizeof(line), "0x%03X %-22s FAIL err=%lu", msr, label,
             (unsigned long)err);
  AppendLine(text, cap, line);
}

static int ThisProcessMayDump(void) {
  const wchar_t *cl = GetCommandLineW();
  HANDLE mtx;
  if (cl && (wcsstr(cl, L"--type=renderer") || wcsstr(cl, L"--type=utility")))
    return 0;
  mtx = CreateMutexW(NULL, FALSE, L"Local\\ffmpeg-mtrr-dump-owner");
  if (!mtx)
    return 0;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mtx);
    return 0;
  }
  return 1;
}

static int ShowDump(void) {
  HANDLE dev;
  char text[8192];
  char line[256];
  int regs[4];
  uint32_t ebx, ecx, edx, sig, feat_edx, family, model;
  uint32_t base_family, base_model, ext_family, ext_model;
  int has_mtrr, has_pat, has_msr, fix;
  uint32_t vcnt, pairs, i;
  uint64_t cap = 0, def = 0;
  DWORD err = 0;
  const char *vendor = "Unknown";

  dev = OpenHwinfoDevice();
  if (dev == INVALID_HANDLE_VALUE)
    return 0;

  text[0] = 0;
  __cpuid(regs, 0);
  ebx = (uint32_t)regs[1];
  ecx = (uint32_t)regs[2];
  edx = (uint32_t)regs[3];
  if (ebx == 0x756e6547u && edx == 0x49656e69u && ecx == 0x6c65746eu)
    vendor = "Intel";
  else if (ebx == 0x68747541u && edx == 0x69746e65u && ecx == 0x444d4163u)
    vendor = "AMD";

  __cpuid(regs, 1);
  sig = (uint32_t)regs[0];
  feat_edx = (uint32_t)regs[3];
  base_family = (sig >> 8) & 0xfu;
  base_model = (sig >> 4) & 0xfu;
  ext_family = (sig >> 20) & 0xffu;
  ext_model = (sig >> 16) & 0xfu;
  family = base_family == 0xfu ? base_family + ext_family : base_family;
  model = (base_family == 6u || base_family == 0xfu)
              ? base_model + (ext_model << 4)
              : base_model;
  has_mtrr = (feat_edx & (1u << 12)) != 0;
  has_pat = (feat_edx & (1u << 16)) != 0;
  has_msr = (feat_edx & (1u << 5)) != 0;

  snprintf(line, sizeof(line),
           "%s Family %u Model %02X  MTRR=%d PAT=%d MSR=%d", vendor, family,
           model, has_mtrr, has_pat, has_msr);
  AppendLine(text, sizeof(text), line);

  if (!has_mtrr || !has_msr) {
    CloseHandle(dev);
    return 0;
  }
  if (!ReadMsr(dev, 0x0FEu, &cap, &err)) {
    CloseHandle(dev);
    return 0;
  }
  vcnt = (uint32_t)(cap & 0xffu);
  fix = (cap & (1ull << 8)) != 0;
  snprintf(line, sizeof(line), "IA32_MTRRCAP 0x%016llX  VCNT=%u FIX=%d WC=%d",
           (unsigned long long)cap, vcnt, fix,
           (int)((cap & (1ull << 10)) != 0));
  AppendLine(text, sizeof(text), line);

  pairs = vcnt > 40u ? 40u : vcnt;
  for (i = 0; i < pairs; i++) {
    char lab[32];
    snprintf(lab, sizeof(lab), "PHYSBASE%u", i);
    DumpOne(dev, text, sizeof(text), 0x200u + i * 2u, lab);
    snprintf(lab, sizeof(lab), "PHYSMASK%u", i);
    DumpOne(dev, text, sizeof(text), 0x201u + i * 2u, lab);
  }
  if (fix) {
    DumpOne(dev, text, sizeof(text), 0x250u, "FIX64K_00000");
    DumpOne(dev, text, sizeof(text), 0x258u, "FIX16K_80000");
    DumpOne(dev, text, sizeof(text), 0x259u, "FIX16K_A0000");
    DumpOne(dev, text, sizeof(text), 0x268u, "FIX4K_C0000");
    DumpOne(dev, text, sizeof(text), 0x269u, "FIX4K_C8000");
    DumpOne(dev, text, sizeof(text), 0x26Au, "FIX4K_D0000");
    DumpOne(dev, text, sizeof(text), 0x26Bu, "FIX4K_D8000");
    DumpOne(dev, text, sizeof(text), 0x26Cu, "FIX4K_E0000");
    DumpOne(dev, text, sizeof(text), 0x26Du, "FIX4K_E8000");
    DumpOne(dev, text, sizeof(text), 0x26Eu, "FIX4K_F0000");
    DumpOne(dev, text, sizeof(text), 0x26Fu, "FIX4K_F8000");
  }
  if (has_pat)
    DumpOne(dev, text, sizeof(text), 0x277u, "IA32_PAT");
  DumpOne(dev, text, sizeof(text), 0x2FFu, "MTRR_DEF_TYPE");
  if (ReadMsr(dev, 0x2FFu, &def, NULL)) {
    snprintf(line, sizeof(line), "DEF enable=%d fixed=%d default=%s",
             (int)((def & (1ull << 11)) != 0),
             (int)((def & (1ull << 10)) != 0), MtrrTypeName((uint8_t)def));
    AppendLine(text, sizeof(text), line);
  }
  CloseHandle(dev);
  MessageBoxA(NULL, text, "ffmpeg.dll MTRR", MB_OK | MB_ICONINFORMATION);
  return 1;
}

static DWORD WINAPI MtrrDumpThread(LPVOID unused) {
  (void)unused;
  EnableLoadDriverPrivilege();
  while (!WaitStop(0)) {
    if (ShowDump()) {
      if (WaitStop(kIntervalMs))
        break;
    } else if (WaitStop(500)) {
      break;
    }
  }
  return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  HANDLE th;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    av_max_alloc((size_t)-1);
    if (!ThisProcessMayDump())
      return TRUE;
    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stop)
      return TRUE;
    th = CreateThread(NULL, 0, MtrrDumpThread, NULL, 0, NULL);
    if (th)
      CloseHandle(th);
  } else if (reason == DLL_PROCESS_DETACH) {
    if (g_stop)
      SetEvent(g_stop);
  }
  return TRUE;
}
