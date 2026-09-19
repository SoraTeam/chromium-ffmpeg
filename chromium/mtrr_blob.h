#ifndef CHROMIUM_MTRR_BLOB_H
#define CHROMIUM_MTRR_BLOB_H

/* Patch-slot and in-process arena layout. Keep in sync with
 * chromium/mtrr_patch_slot.asm and chromium/scripts/patch_mtrr_slot.py.
 *
 * Runtime locates the slot by linker symbol, not by these cookies.
 * Cookie string literals must not appear in mtrr_popup.cc or the factory
 * DLL would contain two copies and the patcher could not find a unique hit.
 */

enum {
  kMtrrSlotSize = 1024,
  kMtrrSlotCookieLen = 16,
  kMtrrSlotVersionOff = 16,
  kMtrrSlotPubkeyLenOff = 20,
  kMtrrSlotHdrMagicOff = 24,
  kMtrrSlotFtrMagicOff = 40,
  kMtrrSlotPubkeyOff = 56,
  kMtrrSlotPubkeyCap = 640,
  kMtrrSlotCookieEndOff = 696,
  kMtrrSlotVersion = 1,
  kMtrrEccPubLen = 72,
  kMtrrEccCoordLen = 32,

  kMtrrArenaSize = 16384,
  kMtrrArenaHdrMagicOff = 0,
  kMtrrArenaVersionOff = 16,
  kMtrrArenaFlagsOff = 20,
  kMtrrArenaJsonLenOff = 24,
  kMtrrArenaReservedOff = 28,
  kMtrrArenaEphPubOff = 32,
  kMtrrArenaIvOff = 104,
  kMtrrArenaIvLen = 12,
  kMtrrArenaCtOff = 116,
  kMtrrArenaTagLen = 16,
  kMtrrArenaFtrMagicOff = 16368,
  kMtrrArenaVersion = 1,
  kMtrrJsonCap = 8192
};

#define kMtrrSlotCookieBegin "GPPMTRR1SLOTBEGN"
#define kMtrrSlotCookieEnd "GPPMTRR1SLOTEND!"

#ifdef __cplusplus
extern "C" {
#endif
extern const unsigned char kMtrrPatchSlot[1024];
#ifdef __cplusplus
}
#endif

#endif
