/* Host stand-in for the edk2 AutoGen.h of the modules under test.
 * PCD values are copied from the 6943 build's AutoGen.h files preserved as
 * reference-6943-*-AutoGen.h next to this file. Host test only. */
#ifndef S1_HOST_AUTOGEN_H
#define S1_HOST_AUTOGEN_H
#include <Uefi.h>
#include <Library/PcdLib.h>

extern EFI_GUID  gEfiCallerIdGuid;
extern EFI_GUID  gNVMeEnableStartEventGroupGuid;
extern EFI_GUID  gNVMeEnableCompleteEventGroupGuid;
extern EFI_GUID  gEdkiiNonDiscoverableAhciDeviceGuid;
extern EFI_GUID  gEdkiiNonDiscoverableAmbaDeviceGuid;
extern EFI_GUID  gEdkiiNonDiscoverableEhciDeviceGuid;
extern EFI_GUID  gEdkiiNonDiscoverableNvmeDeviceGuid;
extern EFI_GUID  gEdkiiNonDiscoverableOhciDeviceGuid;
extern EFI_GUID  gEdkiiNonDiscoverableSdhciDeviceGuid;
extern EFI_GUID  gEdkiiNonDiscoverableUfsDeviceGuid;
extern EFI_GUID  gEdkiiNonDiscoverableUhciDeviceGuid;
extern EFI_GUID  gEdkiiNonDiscoverableXhciDeviceGuid;
extern EFI_GUID  gEfiEndOfDxeEventGroupGuid;

/* NvmExpressDxe (reference-6943-NvmExpressDxe-AutoGen.h) */
#define _PCD_VALUE_PcdNvmeGenericTimeout                   5U
#define _PCD_GET_MODE_32_PcdNvmeGenericTimeout             5U
#define _PCD_VALUE_PcdSupportAlternativeQueueSize          0U
#define _PCD_GET_MODE_BOOL_PcdSupportAlternativeQueueSize  ((BOOLEAN)0U)
#define _PCD_GET_MODE_32_PcdNvmeNamespaceFilterId          0U

/* J716sSyntheticNvmeDxe compiles but its S1Entry is not called here. */
#define _PCD_VALUE_PcdBootArgsPointer  0ULL
#define _PCD_VALUE_PcdAdtPointer       0ULL
#define _PCD_VALUE_PcdJ716sGicRoute    ((BOOLEAN)1)
#endif
