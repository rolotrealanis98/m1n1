/* Actual-code host tests for the S1 binding fix.
 *
 * Linked actual code (unmodified apart from the reviewed patch 0003):
 *   - J716sSyntheticNvmeDxe.c (#included for its STATIC EndOfDxe/Probe)
 *   - NonDiscoverableDeviceRegistrationLib.c
 *   - NonDiscoverablePciDeviceDxe.c + NonDiscoverablePciDeviceIo.c (facade)
 *   - NvmExpressDxe: NvmExpress.c, NvmExpressHci.c, NvmExpressPassthru.c,
 *     NvmExpressBlockIo.c, NvmExpressDiskInfo.c, NvmExpressMediaSanitize.c
 *   - frozen resident S1 controller model (resident_bridge.c)
 * Host substitutions: fake_uefi.c (handle DB, boot services, base libs) and
 * the facade's two BAR MMIO accessors, which are re-pointed to the resident
 * model at the same BAR offsets (the facade's own accessors are plain
 * volatile loads/stores to Desc->AddrRangeMin + Offset). No hardware.
 *
 * Build: S1_FACADE_FLAG=1 (DSC J716S_SYNTHETIC_NVME_FACADE_ID) or 0. */
#include "J716sSyntheticNvmeDxe.c"
#include <Protocol/DriverBinding.h>
#include <Protocol/PciIo.h>
#include <IndustryStandard/Pci.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char   gLog[];
extern int    gAsserts, gVerbose, gBindingCount, gFailAllocatePagesAfter;
extern UINT64 gTimeouts;
extern EFI_DRIVER_BINDING_PROTOCOL  *gBindings[];
void FakeReset (void);
int  bridge_reset (int deny);
int  bridge_mmio (UINT64 addr, unsigned bytes, int write, UINT64 *value);
int  bridge_config_read (UINT64 off, unsigned bytes, UINT64 *value);
void bridge_stats (UINT64 out[8]);
EFI_STATUS EFIAPI NonDiscoverablePciDeviceDxeEntryPoint (EFI_HANDLE, EFI_SYSTEM_TABLE *);
EFI_STATUS EFIAPI NvmExpressDriverEntry (EFI_HANDLE, EFI_SYSTEM_TABLE *);
extern EFI_GUID  gEfiCallerIdGuid;

static unsigned  Checks, Failures;
#define C(x)  do { Checks++; if (!(x)) { Failures++; fprintf (stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x); } } while (0)
#define HAS(s)  (strstr (gLog, (s)) != NULL)

static EFI_DRIVER_BINDING_PROTOCOL  *Facade, *Nvme;
static unsigned long                Mmio, MmioRejected;
static EFI_STATUS                   LastNvmeStart;
static int                          FailFreeBuffer;
static EFI_PCI_IO_PROTOCOL_FREE_BUFFER  OrigFreeBuffer;

static EFI_STATUS
Rw (int Write, EFI_PCI_IO_PROTOCOL_WIDTH W, UINT8 Bar, UINT64 Off, UINTN Count, VOID *Buf)
{
  unsigned  Bytes = 1u << (W & 3);
  UINTN     I;
  UINT64    V;
  if ((Bar != 0) || (W > EfiPciIoWidthUint64) || (Off + Count * Bytes > S1_BAR_BYTES)) {
    return EFI_UNSUPPORTED;
  }
  for (I = 0; I < Count; I++) {
    V = 0;
    if (Write) memcpy (&V, (UINT8 *)Buf + I * Bytes, Bytes);
    Mmio++;
    if (!bridge_mmio (S1_BAR + Off + I * Bytes, Bytes, Write, &V)) {
      /* The live resident treats a rejected access as a guest fault. */
      MmioRejected++;
      return EFI_DEVICE_ERROR;
    }
    if (!Write) memcpy ((UINT8 *)Buf + I * Bytes, &V, Bytes);
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TRead (EFI_PCI_IO_PROTOCOL *P, EFI_PCI_IO_PROTOCOL_WIDTH W, UINT8 B, UINT64 O, UINTN C, VOID *Buf) { return Rw (0, W, B, O, C, Buf); }
static EFI_STATUS EFIAPI TWrite (EFI_PCI_IO_PROTOCOL *P, EFI_PCI_IO_PROTOCOL_WIDTH W, UINT8 B, UINT64 O, UINTN C, VOID *Buf) { return Rw (1, W, B, O, C, Buf); }
static EFI_STATUS EFIAPI FailingFree (EFI_PCI_IO_PROTOCOL *P, UINTN Pages, VOID *Host)
{ OrigFreeBuffer (P, Pages, Host); return EFI_DEVICE_ERROR; }

EFI_STATUS EFIAPI
HarnessConnectController (EFI_HANDLE Controller, EFI_HANDLE *Drv, EFI_DEVICE_PATH_PROTOCOL *Rem, BOOLEAN Recursive)
{
  EFI_STATUS           S1, S2;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  BOOLEAN              Started = FALSE;

  S1 = Facade->Supported (Facade, Controller, NULL);
  if (!EFI_ERROR (S1)) {
    S1 = Facade->Start (Facade, Controller, NULL);
    if (!EFI_ERROR (S1)) {
      Started = TRUE;
      if (!EFI_ERROR (gBS->HandleProtocol (Controller, &gEfiPciIoProtocolGuid, (VOID **)&PciIo))) {
        PciIo->Mem.Read  = TRead;
        PciIo->Mem.Write = TWrite;
        if (FailFreeBuffer) { OrigFreeBuffer = PciIo->FreeBuffer; PciIo->FreeBuffer = FailingFree; }
      }
    }
  }
  LastNvmeStart = EFI_NOT_STARTED;
  S2 = Nvme->Supported (Nvme, Controller, NULL);
  if (!EFI_ERROR (S2)) {
    LastNvmeStart = Nvme->Start (Nvme, Controller, NULL);
    if (!EFI_ERROR (LastNvmeStart)) Started = TRUE;
  }
  /* Children (BlockIo/partition) are not recursively connected here. */
  return Started ? EFI_SUCCESS : EFI_NOT_FOUND;
}

static void
Setup (int Deny)
{
  EFI_HANDLE  I1 = NULL, I2 = NULL;
  static int  Dummy;
  FakeReset ();
  gBindingCount = 0; Mmio = MmioRejected = 0; FailFreeBuffer = 0; gTimeouts = 0;
  gBS->InstallProtocolInterface (&I1, &gEfiCallerIdGuid, EFI_NATIVE_INTERFACE, &Dummy);
  gBS->InstallProtocolInterface (&I2, &gEfiCallerIdGuid, EFI_NATIVE_INTERFACE, &Dummy);
  NonDiscoverablePciDeviceDxeEntryPoint (I1, NULL);
  NvmExpressDriverEntry (I2, NULL);
  Facade = gBindings[0]; Nvme = gBindings[1];
  if (!bridge_reset (Deny)) { fprintf (stderr, "resident model setup failed\n"); exit (2); }
  mController = NULL;
}

static void
ReadIds (EFI_HANDLE H, UINT16 *Vid, UINT16 *Did, UINT8 Cls[3])
{
  EFI_PCI_IO_PROTOCOL  *PciIo;
  UINT16               VidDid[2] = { 0x1234, 0x5678 };
  *Vid = *Did = 0x1234;
  if (EFI_ERROR (gBS->HandleProtocol (H, &gEfiPciIoProtocolGuid, (VOID **)&PciIo))) return;
  /* Same call as NvmeControllerInit's VID/DID check. */
  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint16, PCI_VENDOR_ID_OFFSET, 2, VidDid);
  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint8, PCI_CLASSCODE_OFFSET, 3, Cls);
  *Vid = VidDid[0]; *Did = VidDid[1];
}

static int
HasPassThru (EFI_HANDLE H)
{
  VOID *P;
  return !EFI_ERROR (gBS->HandleProtocol (H, &gEfiNvmExpressPassThruProtocolGuid, &P));
}

/* Exactly the attempt-43 production sequence: EndOfDxe registers UniqueId 16
 * and connects the exact handle; the probe then looks for PassThru on it. */
static void
TestProductionSequence (void)
{
  UINT64  St[8], Cfg0 = 0, Cfg8 = 0;
  UINT16  Vid, Did;
  UINT8   Cls[3];

  Setup (0);
  EndOfDxe (NULL, NULL);
  bridge_stats (St);
  ReadIds (mController, &Vid, &Did, Cls);
  C (Cls[0] == 0x02 && Cls[1] == 0x08 && Cls[2] == 0x01);
  C (HAS ("S1_MU: synthetic controller connection: Success"));
  C (!HAS ("end with Success"));
  C (MmioRejected == 0);
  printf ("  production sequence (flag %d): VID %04x DID %04x, NvmExpress start %s, MMIO %lu, resident commands %llu, reads %llu\n",
    S1_FACADE_FLAG, Vid, Did, EFI_ERROR (LastNvmeStart) ? "failed" : "ok", Mmio,
    (unsigned long long)St[1], (unsigned long long)St[0]);
#if S1_FACADE_FLAG
  bridge_config_read (0, 4, &Cfg0);
  bridge_config_read (8, 4, &Cfg8);
  C (Vid == 0x1b36 && Did == 0x0010);
  C (Cfg0 == (((UINT64)Did << 16) | Vid));                         /* facade == resident identity */
  C ((Cfg8 >> 8) == (((UINT64)Cls[2] << 16) | (Cls[1] << 8) | Cls[0])); /* facade == resident class */
  C (HAS ("InitializePciIoProtocol: J716s synthetic NVMe facade UniqueId 16 identity 1b36:0010"));
  C (LastNvmeStart == EFI_SUCCESS);
  C (HAS ("NvmExpressDriverBindingStart: end successfully"));
  C (!HAS ("NvmExpressDriverBindingStart: start failed"));
  C (!HAS ("end with"));
  C (HasPassThru (mController));
  C (HAS ("S1_MU: READ 0 exact 4096-byte fixture PASS"));
  C (HAS ("S1_MU: READ 39 exact 4096-byte fixture PASS"));
  C (HAS ("S1_MU: DONE Identify40x4096 READ0 READ39; no disk writes issued by probe"));
  C (!HAS ("S1_MU: no controller PassThru"));
  C (St[0] == 2);                       /* exactly the probe's LBA0 and LBA39 */
  C (St[1] == St[2]);                   /* every command completed */
  C (St[4] == 0 && St[5] == 0 && St[6] == 0);
  C ((St[3] & 1) == 1);                 /* CSTS.RDY */
  C ((St[3] >> 32) == 0x00460001);      /* CC: EN, IOSQES 6, IOCQES 4 */
  C (Mmio > 0);
  C (gAsserts == 0);
  C (gTimeouts == 0);
#else
  /* Default-off build: reproduces attempt 43 except the corrected status. */
  C (Vid == 0xffff && Did == 0x0000);
  C (!HAS ("J716s synthetic NVMe facade"));
  C (LastNvmeStart == EFI_DEVICE_ERROR);
  C (HAS ("NvmExpressDriverBindingStart: start failed Device Error; queue cleanup Success"));
  C (HAS ("NvmExpressDriverBindingStart: end with Device Error"));
  C (!HAS ("end successfully"));
  C (!HasPassThru (mController));
  C (HAS ("S1_MU: no controller PassThru: Unsupported"));
  C (!HAS ("S1_MU: DONE"));
  C (Mmio == 0);                        /* rejected before any MMIO, as in attempt 43 */
  C (St[1] == 0 && St[0] == 0);
  C (gAsserts == 0);
#endif
}

typedef struct {
  const char  *Name;
  UINTN       Uid;
  NON_DISCOVERABLE_DEVICE_TYPE  Type;
  NON_DISCOVERABLE_DEVICE_DMA_TYPE  Dma;
  int         Init;
  UINTN       N;
  UINT64      B0, S0, B1, S1;
  int         Mutate;   /* 1 gran=32, 2 translation, 3 genflag, 4 end checksum, 5 rangemax */
  int         Exact;
  UINT8       Cls[3];
} CASE;

static EFI_STATUS EFIAPI DummyInit (NON_DISCOVERABLE_DEVICE *D) { return EFI_SUCCESS; }

static void
TestIdentityMatrix (void)
{
  static const CASE  Cases[] = {
    { "exact S1 facade", 16, NonDiscoverableDeviceTypeNvme, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR, S1_BAR_BYTES, 0, 0, 0, 1, { 2, 8, 1 } },
    { "UniqueId 17", 17, NonDiscoverableDeviceTypeNvme, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR, S1_BAR_BYTES, 0, 0, 0, 0, { 2, 8, 1 } },
    { "UniqueId 15", 15, NonDiscoverableDeviceTypeNvme, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR, S1_BAR_BYTES, 0, 0, 0, 0, { 2, 8, 1 } },
    { "UniqueId 0", 0, NonDiscoverableDeviceTypeNvme, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR, S1_BAR_BYTES, 0, 0, 0, 0, { 2, 8, 1 } },
    { "BAR +16MiB", 16, NonDiscoverableDeviceTypeNvme, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR + S1_BAR_BYTES, S1_BAR_BYTES, 0, 0, 0, 0, { 2, 8, 1 } },
    { "BAR -16MiB", 16, NonDiscoverableDeviceTypeNvme, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR - S1_BAR_BYTES, S1_BAR_BYTES, 0, 0, 0, 0, { 2, 8, 1 } },
    { "BAR +4KiB", 16, NonDiscoverableDeviceTypeNvme, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR + 0x1000, S1_BAR_BYTES, 0, 0, 0, 0, { 2, 8, 1 } },
    { "size 8MiB", 16, NonDiscoverableDeviceTypeNvme, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR, S1_BAR_BYTES / 2, 0, 0, 0, 0, { 2, 8, 1 } },
    { "size 32MiB", 16, NonDiscoverableDeviceTypeNvme, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR, S1_BAR_BYTES * 2, 0, 0, 0, 0, { 2, 8, 1 } },
    { "non-coherent DMA", 16, NonDiscoverableDeviceTypeNvme, NonDiscoverableDeviceDmaTypeNonCoherent, 0, 1, S1_BAR, S1_BAR_BYTES, 0, 0, 0, 0, { 2, 8, 1 } },
    { "Initialize callback", 16, NonDiscoverableDeviceTypeNvme, NonDiscoverableDeviceDmaTypeCoherent, 1, 1, S1_BAR, S1_BAR_BYTES, 0, 0, 0, 0, { 2, 8, 1 } },
    { "two resources", 16, NonDiscoverableDeviceTypeNvme, NonDiscoverableDeviceDmaTypeCoherent, 0, 2, S1_BAR, S1_BAR_BYTES, 0xF31000000ULL, 0x1000, 0, 0, { 2, 8, 1 } },
    { "32-bit low BAR", 16, NonDiscoverableDeviceTypeNvme, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, 0x30000000, S1_BAR_BYTES, 0, 0, 0, 0, { 2, 8, 1 } },
    { "granularity 32", 16, NonDiscoverableDeviceTypeNvme, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR, S1_BAR_BYTES, 0, 0, 1, 0, { 2, 8, 1 } },
    { "translation offset", 16, NonDiscoverableDeviceTypeNvme, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR, S1_BAR_BYTES, 0, 0, 2, 0, { 2, 8, 1 } },
    { "GenFlag", 16, NonDiscoverableDeviceTypeNvme, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR, S1_BAR_BYTES, 0, 0, 3, 0, { 2, 8, 1 } },
    { "end checksum", 16, NonDiscoverableDeviceTypeNvme, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR, S1_BAR_BYTES, 0, 0, 4, 0, { 2, 8, 1 } },
    { "AddrRangeMax", 16, NonDiscoverableDeviceTypeNvme, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR, S1_BAR_BYTES, 0, 0, 5, 0, { 2, 8, 1 } },
    { "xHCI at exact S1 identity", 16, NonDiscoverableDeviceTypeXhci, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR, S1_BAR_BYTES, 0, 0, 0, 0, { 0x30, 3, 0x0c } },
    { "USB xHCI port 0 (J716sUsbHostDxe shape)", 0, NonDiscoverableDeviceTypeXhci, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, 0x702280000ULL, 0x100000, 0, 0, 0, 0, { 0x30, 3, 0x0c } },
    { "USB xHCI port 1", 1, NonDiscoverableDeviceTypeXhci, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, 0xB02280000ULL, 0x100000, 0, 0, 0, 0, { 0x30, 3, 0x0c } },
    { "EHCI", 16, NonDiscoverableDeviceTypeEhci, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR, S1_BAR_BYTES, 0, 0, 0, 0, { 0x20, 3, 0x0c } },
    { "OHCI", 16, NonDiscoverableDeviceTypeOhci, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR, S1_BAR_BYTES, 0, 0, 0, 0, { 0x10, 3, 0x0c } },
    { "UHCI", 16, NonDiscoverableDeviceTypeUhci, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR, S1_BAR_BYTES, 0, 0, 0, 0, { 0x00, 3, 0x0c } },
    { "SDHCI", 16, NonDiscoverableDeviceTypeSdhci, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR, S1_BAR_BYTES, 0, 0, 0, 0, { 0x00, 5, 0x08 } },
    { "UFS", 16, NonDiscoverableDeviceTypeUfs, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, S1_BAR, S1_BAR_BYTES, 0, 0, 0, 0, { 0x00, 9, 0x01 } },
    { "AHCI (32-bit)", 16, NonDiscoverableDeviceTypeAhci, NonDiscoverableDeviceDmaTypeCoherent, 0, 1, 0x30000000, 0x1000, 0, 0, 0, 0, { 0x01, 6, 0x01 } },
  };
  unsigned  I, Exact = 0, Other = 0;
  for (I = 0; I < sizeof Cases / sizeof Cases[0]; I++) {
    const CASE               *K = &Cases[I];
    EFI_HANDLE               H = NULL;
    NON_DISCOVERABLE_DEVICE  *Dev;
    EFI_STATUS               S;
    UINT16                   Vid, Did;
    UINT8                    Cls[3];
    int                      WantExact = K->Exact && S1_FACADE_FLAG;

    Setup (0);
    if (K->N == 1) {
      S = RegisterNonDiscoverableMmioDevice (K->Uid, K->Type, K->Dma, K->Init ? DummyInit : NULL, &H, 1, (UINTN)K->B0, (UINTN)K->S0);
    } else {
      S = RegisterNonDiscoverableMmioDevice (K->Uid, K->Type, K->Dma, K->Init ? DummyInit : NULL, &H, 2, (UINTN)K->B0, (UINTN)K->S0, (UINTN)K->B1, (UINTN)K->S1);
    }
    C (S == EFI_SUCCESS);
    gBS->HandleProtocol (H, &gEdkiiNonDiscoverableDeviceProtocolGuid, (VOID **)&Dev);
    switch (K->Mutate) {
      case 1: Dev->Resources[0].AddrSpaceGranularity = 32; break;
      case 2: Dev->Resources[0].AddrTranslationOffset = 0x1000; break;
      case 3: Dev->Resources[0].GenFlag = 1; break;
      case 4: ((EFI_ACPI_END_TAG_DESCRIPTOR *)&Dev->Resources[1])->Checksum = 1; break;
      case 5: Dev->Resources[0].AddrRangeMax -= 1; break;
    }
    S = Facade->Start (Facade, H, NULL);
    C (S == EFI_SUCCESS);
    ReadIds (H, &Vid, &Did, Cls);
    C (memcmp (Cls, K->Cls, 3) == 0);
    if (WantExact) {
      C (Vid == 0x1b36 && Did == 0x0010);
      C (HAS ("J716s synthetic NVMe facade UniqueId 16 identity 1b36:0010"));
      Exact++;
    } else {
      C (Vid == 0xffff && Did == 0x0000);
      C (!HAS ("J716s synthetic NVMe facade"));
      Other++;
    }
    C (gAsserts == 0);
    if (gVerbose) printf ("  %-40s -> %04x:%04x class %02x%02x%02x\n", K->Name, Vid, Did, Cls[2], Cls[1], Cls[0]);
  }
  printf ("  identity matrix: %u exact 1b36:0010, %u unchanged ffff:0000\n", Exact, Other);
}

#if S1_FACADE_FLAG
/* Failures reached only now that init proceeds past the VID check. */
static void
TestInitFailurePropagation (void)
{
  UINT64  St[8];

  /* (a) admin queue AllocateBuffer fails after CAP was read via MMIO. */
  Setup (0);
  gFailAllocatePagesAfter = 0;
  EndOfDxe (NULL, NULL);
  bridge_stats (St);
  C (LastNvmeStart == EFI_OUT_OF_RESOURCES);
  C (HAS ("NvmExpressDriverBindingStart: start failed Out of Resources; queue cleanup Success"));
  C (HAS ("NvmExpressDriverBindingStart: end with Out of Resources"));
  C (!HAS ("end with Success") && !HAS ("end successfully"));
  C (!HasPassThru (mController));
  C (HAS ("S1_MU: no controller PassThru: Unsupported"));
  C (Mmio > 0 && MmioRejected == 0);   /* got past the VID check into CAP */
  C ((St[3] >> 32) == 0);               /* controller never enabled */

  /* (b) Identify Controller DMA refused by the resident (data transfer
   *     error completion): queues were allocated, cleanup succeeds, the
   *     original EFI_NOT_FOUND must survive. */
  Setup (1);
  EndOfDxe (NULL, NULL);
  bridge_stats (St);
  C (LastNvmeStart == EFI_NOT_FOUND);
  C (St[5] > 0);
  C (HAS ("NvmExpressDriverBindingStart: start failed Not Found; queue cleanup Success"));
  C (HAS ("NvmExpressDriverBindingStart: end with Not Found"));
  C (!HAS ("end with Success") && !HAS ("end successfully"));
  C (!HasPassThru (mController));
  C (HAS ("S1_MU: no controller PassThru: Unsupported"));
  C (gAsserts == 0);

  /* (c) same, and queue cleanup itself fails: original error still wins,
   *     both are logged. */
  Setup (1);
  FailFreeBuffer = 1;
  EndOfDxe (NULL, NULL);
  C (LastNvmeStart == EFI_NOT_FOUND);
  C (HAS ("NvmExpressDriverBindingStart: start failed Not Found; queue cleanup Device Error"));
  C (HAS ("NvmExpressDriverCleanUpQueues failed Device Error"));
  C (HAS ("NvmExpressDriverBindingStart: end with Not Found"));
  C (!HasPassThru (mController));
  C (gAsserts >= 1);                    /* inherited ASSERT_EFI_ERROR in cleanup */
}
#endif

#ifdef S1_WIN_TAKEOVER
int win_takeover (void);
/* (1) facade config view (UEFI consumer) vs resident ECAM view (Windows). */
static void
FacadeVsEcam (void)
{
  EFI_PCI_IO_PROTOCOL  *PciIo;
  UINT32               F[16], E;
  UINT64               V;
  unsigned             I;
  static const char    *Name[16] = { "id", "cmd/sts", "rev/class", "hdr", "BAR0", "BAR1", "BAR2", "BAR3", "BAR4", "BAR5", "cis", "subsys", "rom", "cap", "rsv", "int" };
  C (!EFI_ERROR (gBS->HandleProtocol (mController, &gEfiPciIoProtocolGuid, (VOID **)&PciIo)));
  PciIo->Pci.Read (PciIo, EfiPciIoWidthUint32, 0, 16, F);
  printf ("  facade vs ECAM config dwords (UEFI vs Windows view):\n");
  for (I = 0; I < 16; I++) {
    V = 0; bridge_config_read (I * 4, 4, &V); E = (UINT32)V;
    if (I == 1) { F[I] &= 0xffff0000u; E &= 0xffff0000u; } /* command differs by owner state */
    printf ("    %02x %-9s facade %08x ecam %08x %s\n", I * 4, Name[I], F[I], E, F[I] == E ? "" : "DIFF");
  }
  C (F[0] == 0x00101b36 && (F[2] >> 8) == 0x010802);   /* identity and class agree */
  C (F[4] == ((UINT32)S1_BAR | 4) && F[5] == (UINT32)(S1_BAR >> 32)); /* BAR agrees */
}
#endif
int
main (void)
{
  gVerbose = getenv ("S1_VERBOSE") != NULL;
  printf ("S1 bindfix actual-code tests, facade flag %d\n", S1_FACADE_FLAG);
  TestProductionSequence ();
#ifdef S1_WIN_TAKEOVER
  if (S1_FACADE_FLAG) { FacadeVsEcam (); Failures += win_takeover (); }
#endif
  TestIdentityMatrix ();
#if S1_FACADE_FLAG
  TestInitFailurePropagation ();
#endif
  if (Failures) { printf ("FAIL %u of %u checks\n", Failures, Checks); return 1; }
  printf ("PASS %u checks (facade flag %d)\n", Checks, S1_FACADE_FLAG);
  return 0;
}
