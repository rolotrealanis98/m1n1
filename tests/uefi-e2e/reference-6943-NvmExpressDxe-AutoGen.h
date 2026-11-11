/**
  DO NOT EDIT
  FILE auto-generated
  Module name:
    AutoGen.h
  Abstract:       Auto-generated AutoGen.h for building module or library.
**/

#ifndef _AUTOGENH_5BE3BDF4_53CF_46a3_A6A9_73C34A6E5EE3
#define _AUTOGENH_5BE3BDF4_53CF_46a3_A6A9_73C34A6E5EE3

#ifdef __cplusplus
extern "C" {
#endif

#include <Uefi.h>
#include <Library/PcdLib.h>

extern GUID  gEfiCallerIdGuid;
extern GUID  gEdkiiDscPlatformGuid;
extern CHAR8 *gEfiCallerBaseName;

#define EFI_CALLER_ID_GUID \
  {0x5BE3BDF4, 0x53CF, 0x46a3, {0xA6, 0xA9, 0x73, 0xC3, 0x4A, 0x6E, 0x5E, 0xE3}}
#define EDKII_DSC_PLATFORM_GUID \
  {0xd35bba83, 0x6e73, 0x4cdc, {0x86, 0xd0, 0x9f, 0x9f, 0xba, 0x55, 0x5d, 0x45}}
#define STACK_COOKIE_VALUE 0x74B2A33EDF0CEE6EULL

// Guids
extern EFI_GUID gNVMeEnableStartEventGroupGuid;
extern EFI_GUID gNVMeEnableCompleteEventGroupGuid;
extern EFI_GUID gEfiMdeModulePkgTokenSpaceGuid;
extern EFI_GUID gEfiMdePkgTokenSpaceGuid;
extern EFI_GUID gAppleSiliconPkgTokenSpaceGuid;
extern EFI_GUID gEfiVTUTF8Guid;
extern EFI_GUID gEfiVT100Guid;
extern EFI_GUID gEfiVT100PlusGuid;
extern EFI_GUID gEfiPcAnsiGuid;
extern EFI_GUID gEfiUartDevicePathGuid;
extern EFI_GUID gEfiSasDevicePathGuid;
extern EFI_GUID gEfiVirtualDiskGuid;
extern EFI_GUID gEfiVirtualCdGuid;
extern EFI_GUID gEfiPersistentVirtualDiskGuid;
extern EFI_GUID gEfiPersistentVirtualCdGuid;
extern EFI_GUID gEfiEventReadyToBootGuid;
extern EFI_GUID gEfiEventAfterReadyToBootGuid;
extern EFI_GUID gEfiEventLegacyBootGuid;
extern EFI_GUID gEfiGlobalVariableGuid;
extern EFI_GUID gEfiAcpi20TableGuid;
extern EFI_GUID gEfiAcpi10TableGuid;
extern EFI_GUID gEfiHobListGuid;
extern EFI_GUID gEfiStatusCodeSpecificDataGuid;
extern EFI_GUID gEfiStatusCodeDataTypeDebugGuid;

// Protocols
extern EFI_GUID gEfiPciIoProtocolGuid;
extern EFI_GUID gEfiDevicePathProtocolGuid;
extern EFI_GUID gEfiNvmExpressPassThruProtocolGuid;
extern EFI_GUID gEfiBlockIoProtocolGuid;
extern EFI_GUID gEfiBlockIo2ProtocolGuid;
extern EFI_GUID gEfiDiskInfoProtocolGuid;
extern EFI_GUID gEfiStorageSecurityCommandProtocolGuid;
extern EFI_GUID gEfiDriverSupportedEfiVersionProtocolGuid;
extern EFI_GUID gMediaSanitizeProtocolGuid;
extern EFI_GUID gEfiResetNotificationProtocolGuid;
extern EFI_GUID gPcdProtocolGuid;
extern EFI_GUID gEfiPcdProtocolGuid;
extern EFI_GUID gGetPcdInfoProtocolGuid;
extern EFI_GUID gEfiGetPcdInfoProtocolGuid;
extern EFI_GUID gEfiDebugPortProtocolGuid;
extern EFI_GUID gEfiDriverBindingProtocolGuid;
extern EFI_GUID gEfiSimpleTextOutProtocolGuid;
extern EFI_GUID gEfiGraphicsOutputProtocolGuid;
extern EFI_GUID gEfiHiiFontProtocolGuid;
extern EFI_GUID gEfiSimpleFileSystemProtocolGuid;
extern EFI_GUID gEfiComponentNameProtocolGuid;
extern EFI_GUID gEfiComponentName2ProtocolGuid;
extern EFI_GUID gEfiDriverConfigurationProtocolGuid;
extern EFI_GUID gEfiDriverConfiguration2ProtocolGuid;
extern EFI_GUID gEfiDriverDiagnosticsProtocolGuid;
extern EFI_GUID gEfiDriverDiagnostics2ProtocolGuid;
extern EFI_GUID gEfiLoadedImageProtocolGuid;
extern EFI_GUID gEfiStatusCodeRuntimeProtocolGuid;

// Definition of SkuId Array
extern UINT64 _gPcd_SkuId_Array[];

// Definition of PCDs used in this module

#define _PCD_TOKEN_PcdNvmeGenericTimeout  0U
#define _PCD_SIZE_PcdNvmeGenericTimeout 4
#define _PCD_GET_MODE_SIZE_PcdNvmeGenericTimeout  _PCD_SIZE_PcdNvmeGenericTimeout 
#define _PCD_VALUE_PcdNvmeGenericTimeout  5U
extern const  UINT32  _gPcd_FixedAtBuild_PcdNvmeGenericTimeout;
#define _PCD_GET_MODE_32_PcdNvmeGenericTimeout  _gPcd_FixedAtBuild_PcdNvmeGenericTimeout
//#define _PCD_SET_MODE_32_PcdNvmeGenericTimeout  ASSERT(FALSE)  // It is not allowed to set value for a FIXED_AT_BUILD PCD

#define _PCD_TOKEN_PcdSupportAlternativeQueueSize  0U
#define _PCD_SIZE_PcdSupportAlternativeQueueSize 1
#define _PCD_GET_MODE_SIZE_PcdSupportAlternativeQueueSize  _PCD_SIZE_PcdSupportAlternativeQueueSize 
#define _PCD_VALUE_PcdSupportAlternativeQueueSize  0U
extern const  BOOLEAN  _gPcd_FixedAtBuild_PcdSupportAlternativeQueueSize;
#define _PCD_GET_MODE_BOOL_PcdSupportAlternativeQueueSize  _gPcd_FixedAtBuild_PcdSupportAlternativeQueueSize
//#define _PCD_SET_MODE_BOOL_PcdSupportAlternativeQueueSize  ASSERT(FALSE)  // It is not allowed to set value for a FIXED_AT_BUILD PCD

#define _PCD_TOKEN_PcdNvmeNamespaceFilterId  8U
#define _PCD_GET_MODE_32_PcdNvmeNamespaceFilterId  LibPcdGet32(_PCD_TOKEN_PcdNvmeNamespaceFilterId)
#define _PCD_GET_MODE_SIZE_PcdNvmeNamespaceFilterId  LibPcdGetSize(_PCD_TOKEN_PcdNvmeNamespaceFilterId)
#define _PCD_SET_MODE_32_PcdNvmeNamespaceFilterId(Value)  LibPcdSet32(_PCD_TOKEN_PcdNvmeNamespaceFilterId, (Value))
#define _PCD_SET_MODE_32_S_PcdNvmeNamespaceFilterId(Value)  LibPcdSet32S(_PCD_TOKEN_PcdNvmeNamespaceFilterId, (Value))

// Definition of PCDs used in libraries is in AutoGen.c


EFI_STATUS
EFIAPI
NvmExpressDriverEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  );



EFI_STATUS
EFIAPI
NvmExpressUnload (
  IN EFI_HANDLE        ImageHandle
  );



#ifdef __cplusplus
}
#endif

#endif
