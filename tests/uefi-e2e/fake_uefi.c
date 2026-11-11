/* Minimal host UEFI environment for the actual-code tests: a handle/protocol
 * database, boot services, and the library functions the modules link to.
 * Host only; no hardware. ConnectController is implemented by the harness. */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/ReportStatusCodeLib.h>
#include <Protocol/ComponentName.h>
#include <Protocol/ComponentName2.h>
#include <Protocol/DriverBinding.h>
#include <Pi/PiDxeCis.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- GUIDs (values only need to be distinct on the host) ---- */
#define G(n, a) EFI_GUID n = { a, 0x5331, 0x4d55, { 0x8b, 0x1d, 0, 0, 0, 0, 0, (UINT8)(a) } };
G (gEfiCallerIdGuid, 1)
G (gNVMeEnableStartEventGroupGuid, 2)
G (gNVMeEnableCompleteEventGroupGuid, 3)
G (gEdkiiNonDiscoverableAhciDeviceGuid, 4)
G (gEdkiiNonDiscoverableAmbaDeviceGuid, 5)
G (gEdkiiNonDiscoverableEhciDeviceGuid, 6)
G (gEdkiiNonDiscoverableNvmeDeviceGuid, 7)
G (gEdkiiNonDiscoverableOhciDeviceGuid, 8)
G (gEdkiiNonDiscoverableSdhciDeviceGuid, 9)
G (gEdkiiNonDiscoverableUfsDeviceGuid, 10)
G (gEdkiiNonDiscoverableUhciDeviceGuid, 11)
G (gEdkiiNonDiscoverableXhciDeviceGuid, 12)
G (gEdkiiNonDiscoverableDeviceProtocolGuid, 13)
G (gEfiBlockIo2ProtocolGuid, 14)
G (gEfiBlockIoProtocolGuid, 15)
G (gEfiComponentName2ProtocolGuid, 16)
G (gEfiComponentNameProtocolGuid, 17)
G (gEfiCpuArchProtocolGuid, 18)
G (gEfiDevicePathProtocolGuid, 19)
G (gEfiDiskInfoProtocolGuid, 20)
G (gEfiDriverBindingProtocolGuid, 21)
G (gEfiDriverSupportedEfiVersionProtocolGuid, 22)
G (gEfiEndOfDxeEventGroupGuid, 23)
G (gEfiNvmExpressPassThruProtocolGuid, 24)
G (gEfiPciIoProtocolGuid, 25)
G (gEfiResetNotificationProtocolGuid, 26)
G (gEfiStorageSecurityCommandProtocolGuid, 27)
G (gMediaSanitizeProtocolGuid, 28)
G (gEfiDiskInfoNvmeInterfaceGuid, 29)
G (gEdkiiIoMmuProtocolGuid, 30)

EFI_HANDLE gImageHandle = (EFI_HANDLE)(UINTN)0x1000;
EFI_COMPONENT_NAME_PROTOCOL   gComponentName, gNvmExpressComponentName;
EFI_COMPONENT_NAME2_PROTOCOL  gComponentName2, gNvmExpressComponentName2;
VOID *gDS = NULL;

/* ---- debug capture ---- */
char   gLog[1 << 20];
size_t gLogLen;
int    gAsserts;
int    gVerbose;

static const char *
StatusText (EFI_STATUS S)
{
  switch (S) {
    case EFI_SUCCESS: return "Success";
    case EFI_INVALID_PARAMETER: return "Invalid Parameter";
    case EFI_UNSUPPORTED: return "Unsupported";
    case EFI_BAD_BUFFER_SIZE: return "Bad Buffer Size";
    case EFI_NOT_READY: return "Not Ready";
    case EFI_DEVICE_ERROR: return "Device Error";
    case EFI_OUT_OF_RESOURCES: return "Out of Resources";
    case EFI_NOT_FOUND: return "Not Found";
    case EFI_ACCESS_DENIED: return "Access Denied";
    case EFI_TIMEOUT: return "Time out";
    case EFI_ALREADY_STARTED: return "Already started";
    default: return "Other status";
  }
}

static void
LogAppend (const char *s)
{
  size_t n = strlen (s);
  if (gLogLen + n + 1 < sizeof (gLog)) {
    memcpy (gLog + gLogLen, s, n);
    gLogLen += n;
    gLog[gLogLen] = 0;
  }
  if (gVerbose) fputs (s, stdout);
}

/* edk2 PrintLib subset: %a %s %r %c %d %u %x %X %p with flags/width/l/ll. */
static void
Format (char *out, size_t cap, const char *f, va_list ap)
{
  size_t o = 0;
  char   spec[16], tmp[256];
  while (*f && o + 1 < cap) {
    if (*f != '%') { out[o++] = *f++; continue; }
    const char *start = f++;
    int longs = 0;
    while (*f == '-' || *f == '0' || *f == ' ' || *f == '+' || *f == ',') f++;
    while (*f >= '0' && *f <= '9') f++;
    while (*f == 'l' || *f == 'L') { longs++; f++; }
    char c = *f++;
    size_t sl = (size_t)(f - start);
    if (sl >= sizeof (spec)) sl = sizeof (spec) - 1;
    memcpy (spec, start, sl); spec[sl] = 0;
    /* drop length modifiers from spec, re-add C ones */
    char base[16]; size_t b = 0;
    for (size_t i = 0; i + 1 < sl; i++) if (spec[i] != 'l' && spec[i] != 'L') base[b++] = spec[i];
    base[b] = 0;
    switch (c) {
      case 'a': { const char *s = va_arg (ap, const char *); snprintf (tmp, sizeof tmp, "%s", s ? s : "(null)"); break; }
      case 's': case 'S': { const CHAR16 *s = va_arg (ap, const CHAR16 *); size_t i = 0;
        while (s && s[i] && i + 1 < sizeof tmp) { tmp[i] = (char)s[i]; i++; } tmp[i] = 0; break; }
      case 'r': snprintf (tmp, sizeof tmp, "%s", StatusText ((EFI_STATUS)va_arg (ap, UINTN))); break;
      case 'c': snprintf (tmp, sizeof tmp, "%c", (char)va_arg (ap, int)); break;
      case 'p': snprintf (tmp, sizeof tmp, "%p", va_arg (ap, void *)); break;
      case 'd': case 'i': case 'u': case 'x': case 'X': {
        char fmt[24];
        char cc = (c == 'i') ? 'd' : c;
        snprintf (fmt, sizeof fmt, "%sll%c", base, cc);
        if (longs) snprintf (tmp, sizeof tmp, fmt, (unsigned long long)va_arg (ap, UINT64));
        else if (cc == 'd') snprintf (tmp, sizeof tmp, fmt, (long long)va_arg (ap, int));
        else snprintf (tmp, sizeof tmp, fmt, (unsigned long long)va_arg (ap, unsigned int));
        break; }
      case '%': strcpy (tmp, "%"); break;
      default: snprintf (tmp, sizeof tmp, "<%%%c>", c); break;
    }
    for (size_t i = 0; tmp[i] && o + 1 < cap; i++) out[o++] = tmp[i];
  }
  out[o] = 0;
}

VOID EFIAPI
DebugPrint (UINTN ErrorLevel, CONST CHAR8 *Format_, ...)
{
  char    buf[1024];
  va_list ap;
  va_start (ap, Format_);
  Format (buf, sizeof buf, Format_, ap);
  va_end (ap);
  LogAppend (buf);
}

VOID EFIAPI
DebugAssert (CONST CHAR8 *FileName, UINTN LineNumber, CONST CHAR8 *Description)
{
  char buf[512];
  gAsserts++;
  snprintf (buf, sizeof buf, "ASSERT %s(%lu): %s\n", FileName, (unsigned long)LineNumber, Description);
  LogAppend (buf);
}

BOOLEAN EFIAPI DebugAssertEnabled (VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return TRUE; }
BOOLEAN EFIAPI DebugCodeEnabled (VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN L) { return TRUE; }
BOOLEAN EFIAPI ReportProgressCodeEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI ReportErrorCodeEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI ReportDebugCodeEnabled (VOID) { return FALSE; }
EFI_STATUS EFIAPI ReportStatusCode (EFI_STATUS_CODE_TYPE T, EFI_STATUS_CODE_VALUE V) { return EFI_SUCCESS; }

/* ---- BaseLib / BaseMemoryLib ---- */
VOID *EFIAPI CopyMem (VOID *D, CONST VOID *S, UINTN L) { return memmove (D, S, L); }
VOID *EFIAPI SetMem (VOID *B, UINTN L, UINT8 V) { return memset (B, V, L); }
VOID *EFIAPI ZeroMem (VOID *B, UINTN L) { return memset (B, 0, L); }
BOOLEAN EFIAPI CompareGuid (CONST GUID *A, CONST GUID *B) { return memcmp (A, B, sizeof (GUID)) == 0; }
GUID *EFIAPI CopyGuid (GUID *D, CONST GUID *S) { memcpy (D, S, sizeof (GUID)); return D; }
UINT32 EFIAPI ReadUnaligned32 (CONST UINT32 *P) { UINT32 v; memcpy (&v, P, 4); return v; }
UINT64 EFIAPI ReadUnaligned64 (CONST UINT64 *P) { UINT64 v; memcpy (&v, P, 8); return v; }
UINT32 EFIAPI WriteUnaligned32 (UINT32 *P, UINT32 V) { memcpy (P, &V, 4); return V; }
UINT64 EFIAPI WriteUnaligned64 (UINT64 *P, UINT64 V) { memcpy (P, &V, 8); return V; }
UINT64 EFIAPI LShiftU64 (UINT64 V, UINTN C) { return V << C; }
UINT64 EFIAPI RShiftU64 (UINT64 V, UINTN C) { return V >> C; }
UINT64 EFIAPI MultU64x32 (UINT64 A, UINT32 B) { return A * B; }
UINT64 EFIAPI DivU64x64Remainder (UINT64 A, UINT64 B, UINT64 *R) { if (R) *R = A % B; return A / B; }
LIST_ENTRY *EFIAPI InitializeListHead (LIST_ENTRY *H) { H->ForwardLink = H->BackLink = H; return H; }
LIST_ENTRY *EFIAPI InsertTailList (LIST_ENTRY *H, LIST_ENTRY *E)
{ E->ForwardLink = H; E->BackLink = H->BackLink; H->BackLink->ForwardLink = E; H->BackLink = E; return H; }
LIST_ENTRY *EFIAPI InsertHeadList (LIST_ENTRY *H, LIST_ENTRY *E)
{ E->BackLink = H; E->ForwardLink = H->ForwardLink; H->ForwardLink->BackLink = E; H->ForwardLink = E; return H; }
LIST_ENTRY *EFIAPI RemoveEntryList (CONST LIST_ENTRY *E)
{ E->ForwardLink->BackLink = E->BackLink; E->BackLink->ForwardLink = E->ForwardLink; return E->ForwardLink; }
BOOLEAN EFIAPI IsListEmpty (CONST LIST_ENTRY *H) { return H->ForwardLink == H; }
LIST_ENTRY *EFIAPI GetFirstNode (CONST LIST_ENTRY *H) { return H->ForwardLink; }
LIST_ENTRY *EFIAPI GetNextNode (CONST LIST_ENTRY *H, CONST LIST_ENTRY *N) { return N->ForwardLink; }
BOOLEAN EFIAPI IsNull (CONST LIST_ENTRY *H, CONST LIST_ENTRY *N) { return H == N; }
UINTN EFIAPI UnicodeSPrintAsciiFormat (CHAR16 *B, UINTN S, CONST CHAR8 *F, ...) { if (B && S >= 2) B[0] = 0; return 0; }

/* ---- MemoryAllocationLib ---- */
int gFailAllocatePagesAfter = -1; /* fail injection for gBS->AllocatePages */
VOID *EFIAPI AllocatePool (UINTN S) { return malloc (S ? S : 1); }
VOID *EFIAPI AllocateZeroPool (UINTN S) { return calloc (1, S ? S : 1); }
VOID EFIAPI FreePool (VOID *B) { free (B); }
VOID *EFIAPI AllocateAlignedPages (UINTN P, UINTN A) { void *p = NULL; return posix_memalign (&p, 4096, P * 4096) ? NULL : p; }
VOID EFIAPI FreeAlignedPages (VOID *B, UINTN P) { free (B); }
VOID EFIAPI FreePages (VOID *B, UINTN P) { free (B); }

/* ---- DevicePathLib subset ---- */
UINTN EFIAPI DevicePathNodeLength (CONST VOID *N) { const UINT8 *p = N; return p[2] | (p[3] << 8); }
BOOLEAN EFIAPI IsDevicePathEnd (CONST VOID *N) { const UINT8 *p = N; return p[0] == END_DEVICE_PATH_TYPE && p[1] == END_ENTIRE_DEVICE_PATH_SUBTYPE; }
UINT16 EFIAPI SetDevicePathNodeLength (VOID *N, UINTN L) { UINT8 *p = N; p[2] = (UINT8)L; p[3] = (UINT8)(L >> 8); return (UINT16)L; }
VOID EFIAPI SetDevicePathEndNode (VOID *N) { UINT8 *p = N; p[0] = END_DEVICE_PATH_TYPE; p[1] = END_ENTIRE_DEVICE_PATH_SUBTYPE; p[2] = 4; p[3] = 0; }
EFI_DEVICE_PATH_PROTOCOL *EFIAPI CreateDeviceNode (UINT8 T, UINT8 S, UINT16 L)
{ UINT8 *p; if (L < 4) return NULL; p = calloc (1, L); p[0] = T; p[1] = S; SetDevicePathNodeLength (p, L); return (EFI_DEVICE_PATH_PROTOCOL *)p; }
static UINTN PathSize (CONST EFI_DEVICE_PATH_PROTOCOL *P)
{ const UINT8 *s = (const UINT8 *)P, *q = s; if (!P) return 0; while (!IsDevicePathEnd (q)) q += DevicePathNodeLength (q); return (UINTN)(q - s) + 4; }
EFI_DEVICE_PATH_PROTOCOL *EFIAPI AppendDevicePathNode (CONST EFI_DEVICE_PATH_PROTOCOL *P, CONST EFI_DEVICE_PATH_PROTOCOL *N)
{
  UINTN ps = P ? PathSize (P) - 4 : 0, ns = N ? DevicePathNodeLength (N) : 0;
  UINT8 *r = calloc (1, ps + ns + 4);
  if (ps) memcpy (r, P, ps);
  if (ns) memcpy (r + ps, N, ns);
  SetDevicePathEndNode (r + ps + ns);
  return (EFI_DEVICE_PATH_PROTOCOL *)r;
}

/* ---- UefiLib ---- */
EFI_DRIVER_BINDING_PROTOCOL *gBindings[4];
int                          gBindingCount;
EFI_STATUS EFIAPI
EfiLibInstallDriverBindingComponentName2 (CONST EFI_HANDLE I, CONST EFI_SYSTEM_TABLE *T, EFI_DRIVER_BINDING_PROTOCOL *DB,
  EFI_HANDLE H, CONST EFI_COMPONENT_NAME_PROTOCOL *C, CONST EFI_COMPONENT_NAME2_PROTOCOL *C2)
{
  DB->ImageHandle = I; DB->DriverBindingHandle = H;
  gBindings[gBindingCount++] = DB;
  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI AddUnicodeString2 (CONST CHAR8 *L, CONST CHAR8 *S, EFI_UNICODE_STRING_TABLE **T, CONST CHAR16 *U, BOOLEAN I) { return EFI_SUCCESS; }
EFI_STATUS EFIAPI FreeUnicodeStringTable (EFI_UNICODE_STRING_TABLE *T) { return EFI_SUCCESS; }
EFI_STATUS EFIAPI EfiEventGroupSignal (CONST EFI_GUID *G) { return EFI_SUCCESS; }

/* ---- handle database ---- */
#define MAXH 64
#define MAXP 16
typedef struct { int Used; struct { EFI_GUID G; VOID *I; VOID *ByDriver; } P[MAXP]; int N; } FAKE_HANDLE;
FAKE_HANDLE gHandles[MAXH];

void FakeReset (void) { memset (gHandles, 0, sizeof gHandles); gLogLen = 0; gLog[0] = 0; gAsserts = 0; gFailAllocatePagesAfter = -1; }

static FAKE_HANDLE *H (EFI_HANDLE h) { FAKE_HANDLE *f = (FAKE_HANDLE *)h; return (f >= gHandles && f < gHandles + MAXH && f->Used) ? f : NULL; }
static int Find (FAKE_HANDLE *f, CONST EFI_GUID *g) { for (int i = 0; i < f->N; i++) if (CompareGuid (&f->P[i].G, g)) return i; return -1; }

static EFI_STATUS EFIAPI
Install (EFI_HANDLE *Handle, EFI_GUID *G, EFI_INTERFACE_TYPE T, VOID *I)
{
  FAKE_HANDLE *f = *Handle ? H (*Handle) : NULL;
  if (!*Handle) { for (int i = 0; i < MAXH; i++) if (!gHandles[i].Used) { f = &gHandles[i]; f->Used = 1; break; } *Handle = f; }
  if (!f || Find (f, G) >= 0 || f->N == MAXP) return EFI_INVALID_PARAMETER;
  f->P[f->N].G = *G; f->P[f->N].I = I; f->P[f->N].ByDriver = NULL; f->N++;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI
Uninstall (EFI_HANDLE Handle, EFI_GUID *G, VOID *I)
{
  FAKE_HANDLE *f = H (Handle); int i;
  if (!f || (i = Find (f, G)) < 0) return EFI_NOT_FOUND;
  f->P[i] = f->P[--f->N];
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI
InstallMultiple (EFI_HANDLE *Handle, ...)
{
  va_list ap; EFI_GUID *g; EFI_STATUS s = EFI_SUCCESS;
  va_start (ap, Handle);
  while ((g = va_arg (ap, EFI_GUID *)) != NULL) { VOID *i = va_arg (ap, VOID *); s = Install (Handle, g, EFI_NATIVE_INTERFACE, i); if (EFI_ERROR (s)) break; }
  va_end (ap);
  return s;
}
static EFI_STATUS EFIAPI
UninstallMultiple (EFI_HANDLE Handle, ...)
{
  va_list ap; EFI_GUID *g;
  va_start (ap, Handle);
  while ((g = va_arg (ap, EFI_GUID *)) != NULL) { VOID *i = va_arg (ap, VOID *); Uninstall (Handle, g, i); }
  va_end (ap);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI
HandleProtocol (EFI_HANDLE Handle, EFI_GUID *G, VOID **I)
{
  FAKE_HANDLE *f = H (Handle); int i;
  if (!f || (i = Find (f, G)) < 0) return EFI_UNSUPPORTED;
  *I = f->P[i].I; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI
OpenProtocol (EFI_HANDLE Handle, EFI_GUID *G, VOID **I, EFI_HANDLE Agent, EFI_HANDLE C, UINT32 A)
{
  FAKE_HANDLE *f = H (Handle); int i;
  if (!f || (i = Find (f, G)) < 0) return EFI_UNSUPPORTED;
  if (I) *I = f->P[i].I;
  if (A & EFI_OPEN_PROTOCOL_BY_DRIVER) {
    if (f->P[i].ByDriver == Agent) return EFI_ALREADY_STARTED;
    if (f->P[i].ByDriver) return EFI_ACCESS_DENIED;
    f->P[i].ByDriver = Agent;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI
CloseProtocol (EFI_HANDLE Handle, EFI_GUID *G, EFI_HANDLE Agent, EFI_HANDLE C)
{
  FAKE_HANDLE *f = H (Handle); int i;
  if (!f || (i = Find (f, G)) < 0) return EFI_NOT_FOUND;
  if (f->P[i].ByDriver == Agent) f->P[i].ByDriver = NULL;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI
LocateHandleBuffer (EFI_LOCATE_SEARCH_TYPE T, EFI_GUID *G, VOID *K, UINTN *N, EFI_HANDLE **B)
{
  EFI_HANDLE *r = calloc (MAXH, sizeof (EFI_HANDLE)); UINTN n = 0;
  for (int i = 0; i < MAXH; i++) if (gHandles[i].Used && Find (&gHandles[i], G) >= 0) r[n++] = &gHandles[i];
  if (!n) { free (r); return EFI_NOT_FOUND; }
  *N = n; *B = r; return EFI_SUCCESS;
}
static UINT8 gCpuDummy[256];
static EFI_STATUS EFIAPI
LocateProtocol (EFI_GUID *G, VOID *R, VOID **I)
{
  if (CompareGuid (G, &gEfiCpuArchProtocolGuid)) { *I = gCpuDummy; return EFI_SUCCESS; }
  return EFI_NOT_FOUND;
}

/* ---- events/timers/memory ---- */
typedef struct { UINT32 Type; UINT64 Checks; BOOLEAN Signaled; } FAKE_EVENT;
static EFI_STATUS EFIAPI CreateEvent (UINT32 T, EFI_TPL P, EFI_EVENT_NOTIFY F, VOID *C, EFI_EVENT *E)
{ FAKE_EVENT *e = calloc (1, sizeof *e); e->Type = T; *E = e; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI CreateEventEx (UINT32 T, EFI_TPL P, EFI_EVENT_NOTIFY F, CONST VOID *C, CONST EFI_GUID *G, EFI_EVENT *E)
{ return CreateEvent (T, P, F, (VOID *)C, E); }
static EFI_STATUS EFIAPI SetTimer (EFI_EVENT E, EFI_TIMER_DELAY T, UINT64 D) { if (E) ((FAKE_EVENT *)E)->Checks = 0; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI SignalEvent (EFI_EVENT E) { if (E) ((FAKE_EVENT *)E)->Signaled = TRUE; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI CloseEvent (EFI_EVENT E) { free (E); return EFI_SUCCESS; }
UINT64 gTimeouts;
static EFI_STATUS EFIAPI CheckEvent (EFI_EVENT E)
{
  FAKE_EVENT *e = E;
  if (e->Signaled) return EFI_SUCCESS;
  if (++e->Checks > 200000) { gTimeouts++; return EFI_SUCCESS; } /* host stand-in for timer expiry */
  return EFI_NOT_READY;
}
static EFI_STATUS EFIAPI Stall (UINTN U) { return EFI_SUCCESS; }
static EFI_TPL EFIAPI RaiseTpl (EFI_TPL T) { return TPL_APPLICATION; }
static VOID EFIAPI RestoreTpl (EFI_TPL T) { }
static EFI_STATUS EFIAPI BsAllocatePages (EFI_ALLOCATE_TYPE T, EFI_MEMORY_TYPE M, UINTN P, EFI_PHYSICAL_ADDRESS *A)
{
  void *p = NULL;
  if (gFailAllocatePagesAfter == 0) return EFI_OUT_OF_RESOURCES;
  if (gFailAllocatePagesAfter > 0) gFailAllocatePagesAfter--;
  if (T != AllocateAnyPages || posix_memalign (&p, 4096, P * 4096)) return EFI_OUT_OF_RESOURCES;
  memset (p, 0, P * 4096);
  *A = (EFI_PHYSICAL_ADDRESS)(UINTN)p; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI BsFreePages (EFI_PHYSICAL_ADDRESS A, UINTN P) { free ((void *)(UINTN)A); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI BsAllocatePool (EFI_MEMORY_TYPE T, UINTN S, VOID **B) { *B = malloc (S); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI BsFreePool (VOID *B) { free (B); return EFI_SUCCESS; }
static VOID EFIAPI BsCopyMem (VOID *D, VOID *S, UINTN L) { memmove (D, S, L); }
static VOID EFIAPI BsSetMem (VOID *B, UINTN L, UINT8 V) { memset (B, V, L); }


/* Longest device-path prefix match, as the UEFI core does. */
static EFI_STATUS EFIAPI
LocateDevicePath (EFI_GUID *G, EFI_DEVICE_PATH_PROTOCOL **Path, EFI_HANDLE *Device)
{
  UINTN Best = 0; EFI_HANDLE BestH = NULL;
  UINTN Total = PathSize (*Path) - 4;
  for (int i = 0; i < MAXH; i++) {
    int pi, gi;
    if (!gHandles[i].Used || (gi = Find (&gHandles[i], G)) < 0 || (pi = Find (&gHandles[i], &gEfiDevicePathProtocolGuid)) < 0) continue;
    EFI_DEVICE_PATH_PROTOCOL *P = gHandles[i].P[pi].I;
    UINTN Len = PathSize (P) - 4;
    if (Len <= Total && memcmp (P, *Path, Len) == 0 && (Len >= Best)) { Best = Len; BestH = &gHandles[i]; }
  }
  if (!BestH) return EFI_NOT_FOUND;
  *Device = BestH; *Path = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)*Path + Best);
  return EFI_SUCCESS;
}

extern EFI_STATUS EFIAPI HarnessConnectController (EFI_HANDLE, EFI_HANDLE *, EFI_DEVICE_PATH_PROTOCOL *, BOOLEAN);

static EFI_BOOT_SERVICES mBs = {
  .RaiseTPL = RaiseTpl, .RestoreTPL = RestoreTpl,
  .AllocatePages = BsAllocatePages, .FreePages = BsFreePages,
  .AllocatePool = BsAllocatePool, .FreePool = BsFreePool,
  .CreateEvent = CreateEvent, .SetTimer = SetTimer, .SignalEvent = SignalEvent,
  .CloseEvent = CloseEvent, .CheckEvent = CheckEvent,
  .InstallProtocolInterface = Install, .UninstallProtocolInterface = Uninstall,
  .HandleProtocol = HandleProtocol, .Stall = Stall,
  .ConnectController = HarnessConnectController,
  .OpenProtocol = OpenProtocol, .CloseProtocol = CloseProtocol,
  .LocateHandleBuffer = LocateHandleBuffer, .LocateDevicePath = LocateDevicePath, .LocateProtocol = LocateProtocol,
  .InstallMultipleProtocolInterfaces = InstallMultiple,
  .UninstallMultipleProtocolInterfaces = UninstallMultiple,
  .CopyMem = BsCopyMem, .SetMem = BsSetMem, .CreateEventEx = CreateEventEx,
};
EFI_BOOT_SERVICES *gBS = &mBs;

/* ---- probe-driver library dependencies (S1Entry is not exercised) ---- */
UINTN EFIAPI ArmReadCurrentEL (VOID) { return 0x4; }
