/*******************************************************************************
*
*  (C) COPYRIGHT AUTHORS, 2015 - 2018
*
*  TITLE:       KLDBG.C, based on KDSubmarine by Evilcry
*
*  VERSION:     1.60
*
*  DATE:        24 Oct 2018
*
*  MINIMUM SUPPORTED OS WINDOWS 7
*
* THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
* ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED
* TO THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
* PARTICULAR PURPOSE.
*
*******************************************************************************/
#include "global.h"
#include "hde\hde64.h"

//ObpLookupNamespaceEntry signatures

// 7600, 7601, 9600, 10240
BYTE NamespacePattern[] = {
    0x0F, 0xB6, 0x7A, 0x28, 0x48, 0x8D, 0x05
};

// 9200 (8 failed even here)
BYTE NamespacePattern8[] = {
    0x0F, 0xB6, 0x79, 0x28, 0x48, 0x8D, 0x05
};

//
// Host Server Silo signature patterns
//

//
// PrivateNamespaces redesigned in Windows 10 starting from 10586.
//

BYTE PsGetServerSiloGlobalsPattern_14393[] = {
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x83, 0xF9, 0xFF
};

BYTE PsGetServerSiloGlobalsPattern_15064_16299[] = {
    0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0xC1, 0x48, 0x83, 0xF9, 0xFF
};

//
// lea rax, ObpPrivateNamespaceLookupTable
//
BYTE LeaPattern_PNS[] = {
    0x48, 0x8d, 0x05
};

//KiSystemServiceStartPattern signature

BYTE  KiSystemServiceStartPattern[] = { 0x8B, 0xF8, 0xC1, 0xEF, 0x07, 0x83, 0xE7, 0x20, 0x25, 0xFF, 0x0F, 0x00, 0x00 };

//
// lea r10, KeServiceDescriptorTable
//
BYTE LeaPattern_KiServiceStart[] = {
    0x4c, 0x8d, 0x15
};


#define MM_SYSTEM_RANGE_START_7 0xFFFF080000000000
#define MM_SYSTEM_RANGE_START_8 0xFFFF800000000000

#define TEXT_SECTION ".text"
#define TEXT_SECTION_LEGNTH sizeof(TEXT_SECTION)

#define PAGE_SECTION "PAGE"
#define PAGE_SECTION_LEGNTH sizeof(PAGE_SECTION)


UCHAR ObpInfoMaskToOffset[0x100];

/*
* ObpInitInfoBlockOffsets
*
* Purpose:
*
* Initialize block offfsets table for working with OBJECT_HEADER data.
*
* Note:
*
* ObpInfoMaskToOffset size depends on Windows version (Win7 = 64, Win10 = 256)
*
* 9200 (Windows 8 Blue)
* OBJECT_HEADER_AUDIT_INFO added
*
* 10586 (Windows 10 TH2)
* OBJECT_HEADER_HANDLE_REVOCATION_INFO added (size in ObpInitInfoBlockOffsets = 32)
*
* 14393 (Windows 10 RS1)
* OBJECT_HEADER_EXTENDED_INFO added and replaced OBJECT_HEADER_HANDLE_REVOCATION_INFO in ObpInitInfoBlockOffsets
* size = 16
*
* HANDLE_REVOCATION_INFO moved to OBJECT_FOOTER which is a part of OBJECT_HEADER_EXTENDED_INFO as pointer.
*
*/
VOID ObpInitInfoBlockOffsets()
{
    UCHAR *p = ObpInfoMaskToOffset;
    UINT i;
    UCHAR c;

    i = 0;

    do {
        c = 0;
        if (i & 1)
            c += sizeof(OBJECT_HEADER_CREATOR_INFO);
        if (i & 2)
            c += sizeof(OBJECT_HEADER_NAME_INFO);
        if (i & 4)
            c += sizeof(OBJECT_HEADER_HANDLE_INFO);
        if (i & 8)
            c += sizeof(OBJECT_HEADER_QUOTA_INFO);
        if (i & 0x10)
            c += sizeof(OBJECT_HEADER_PROCESS_INFO);

        if (i & 0x20) {
            // Padding?
            if (g_NtBuildNumber < 9200) {
                c += sizeof(OBJECT_HEADER_PADDING_INFO);
            }
            else {
                c += sizeof(OBJECT_HEADER_AUDIT_INFO);
            }
        }

        //OBJECT_HEADER_EXTENDED_INFO (OBJECT_HEADER_HANDLE_REVOCATION_INFO in 10586)
        if (i & 0x40) {
            if (g_NtBuildNumber == 10586)
                c += sizeof(OBJECT_HEADER_HANDLE_REVOCATION_INFO);
            else
                c += sizeof(OBJECT_HEADER_EXTENDED_INFO);
        }

        if (i & 0x80)
            c += sizeof(OBJECT_HEADER_PADDING_INFO);

        p[i] = c;
        i++;
    } while (i < 256);

    return;
}

/*
* ObGetObjectHeaderOffsetEx
*
* Purpose:
*
* Query requested structure offset for the given mask
*
*/
BYTE ObGetObjectHeaderOffsetEx(
    _In_ BYTE InfoMask,
    _In_ BYTE DesiredHeaderBit
)
{
    return ObpInfoMaskToOffset[InfoMask & (DesiredHeaderBit | (DesiredHeaderBit - 1))];
}

/*
* ObHeaderToNameInfoAddressEx
*
* Purpose:
*
* Calculate address of name structure from object header flags and object address using ObpInfoMaskToOffset.
*
*/
BOOL ObHeaderToNameInfoAddressEx(
    _In_ UCHAR ObjectInfoMask,
    _In_ ULONG_PTR ObjectAddress,
    _Inout_ PULONG_PTR HeaderAddress,
    _In_ BYTE DesiredHeaderBit
)
{
    BYTE      HeaderOffset;
    ULONG_PTR Address;

    if (HeaderAddress == NULL)
        return FALSE;

    if (ObjectAddress < g_kdctx.SystemRangeStart)
        return FALSE;

    HeaderOffset = ObGetObjectHeaderOffsetEx(ObjectInfoMask, DesiredHeaderBit);
    if (HeaderOffset == 0)
        return FALSE;

    Address = ObjectAddress - HeaderOffset;
    if (Address < g_kdctx.SystemRangeStart)
        return FALSE;

    *HeaderAddress = Address;
    return TRUE;
}

/*
* ObGetObjectHeaderOffset
*
* Purpose:
*
* Query requested structure offset for the given mask
*
*
* Object In Memory Disposition (Obsolete, see ObpInitInfoBlockOffsets comments)
*
* POOL_HEADER
* OBJECT_HEADER_PROCESS_INFO
* OBJECT_HEADER_QUOTA_INFO
* OBJECT_HEADER_HANDLE_INFO
* OBJECT_HEADER_NAME_INFO
* OBJECT_HEADER_CREATOR_INFO
* OBJECT_HEADER
*
*/
BYTE ObGetObjectHeaderOffset(
    _In_ BYTE InfoMask,
    _In_ OBJ_HEADER_INFO_FLAG Flag
)
{
    BYTE OffsetMask, HeaderOffset = 0;

    if ((InfoMask & Flag) == 0)
        return 0;

    OffsetMask = InfoMask & (Flag | (Flag - 1));

    if ((OffsetMask & HeaderCreatorInfoFlag) != 0)
        HeaderOffset += (BYTE)sizeof(OBJECT_HEADER_CREATOR_INFO);

    if ((OffsetMask & HeaderNameInfoFlag) != 0)
        HeaderOffset += (BYTE)sizeof(OBJECT_HEADER_NAME_INFO);

    if ((OffsetMask & HeaderHandleInfoFlag) != 0)
        HeaderOffset += (BYTE)sizeof(OBJECT_HEADER_HANDLE_INFO);

    if ((OffsetMask & HeaderQuotaInfoFlag) != 0)
        HeaderOffset += (BYTE)sizeof(OBJECT_HEADER_QUOTA_INFO);

    if ((OffsetMask & HeaderProcessInfoFlag) != 0)
        HeaderOffset += (BYTE)sizeof(OBJECT_HEADER_PROCESS_INFO);

    return HeaderOffset;
}

/*
* ObHeaderToNameInfoAddress
*
* Purpose:
*
* Calculate address of name structure from object header flags and object address
*
*/
BOOL ObHeaderToNameInfoAddress(
    _In_ UCHAR ObjectInfoMask,
    _In_ ULONG_PTR ObjectAddress,
    _Inout_ PULONG_PTR HeaderAddress,
    _In_ OBJ_HEADER_INFO_FLAG InfoFlag
)
{
    BYTE      HeaderOffset;
    ULONG_PTR Address;

    if (HeaderAddress == NULL)
        return FALSE;

    if (ObjectAddress < g_kdctx.SystemRangeStart)
        return FALSE;

    HeaderOffset = ObGetObjectHeaderOffset(ObjectInfoMask, InfoFlag);
    if (HeaderOffset == 0)
        return FALSE;

    Address = ObjectAddress - HeaderOffset;
    if (Address < g_kdctx.SystemRangeStart)
        return FALSE;

    *HeaderAddress = Address;
    return TRUE;
}

/*
* ObHashName
*
* Purpose:
*
* DJB hash function
*
*/
SIZE_T ObHashName(
    _In_ LPWSTR Name
)
{
    SIZE_T Hash = 5381;
    PWCHAR p = Name;

    while (*p)
        Hash = 33 * Hash ^ *p++;

    return Hash;
}

/*
* ObCopyBoundaryDescriptor
*
* Purpose:
*
* Copy boundary descriptor from kernel to user.
* Use supHeapFree to free allocated buffer.
*
*/
NTSTATUS ObCopyBoundaryDescriptor(
    _In_ OBJECT_NAMESPACE_ENTRY *NamespaceLookupEntry,
    _Out_ POBJECT_BOUNDARY_DESCRIPTOR *BoundaryDescriptor,
    _Out_opt_ PULONG BoundaryDescriptorSize
)
{
    ULONG TotalSize;
    ULONG_PTR BoundaryDescriptorAddress;
    OBJECT_BOUNDARY_DESCRIPTOR BoundaryDescriptorHeader, *CopyDescriptor;

    BoundaryDescriptorAddress = (ULONG_PTR)RtlOffsetToPointer(
        NamespaceLookupEntry,
        sizeof(OBJECT_NAMESPACE_ENTRY));

    if (BoundaryDescriptorAddress < g_kdctx.SystemRangeStart)
        return STATUS_INVALID_PARAMETER;

    RtlSecureZeroMemory(&BoundaryDescriptorHeader, sizeof(BoundaryDescriptorHeader));

    //
    // Read header.
    //
    if (!kdReadSystemMemoryEx(BoundaryDescriptorAddress,
        &BoundaryDescriptorHeader,
        sizeof(OBJECT_BOUNDARY_DESCRIPTOR),
        NULL))
    {
        return STATUS_DEVICE_NOT_READY;
    }

    if (BoundaryDescriptorSize)
        *BoundaryDescriptorSize = 0;

    //
    // Validate header data.
    //
    TotalSize = BoundaryDescriptorHeader.TotalSize;

    if (TotalSize < sizeof(OBJECT_BOUNDARY_DESCRIPTOR))
        return STATUS_INVALID_PARAMETER;

    if (BoundaryDescriptorHeader.Version != 1)
        return STATUS_UNKNOWN_REVISION;

    if ((BoundaryDescriptorAddress + TotalSize) < BoundaryDescriptorAddress)
        return STATUS_INVALID_PARAMETER;

    //
    // Dump entire boundary descriptor.
    //
    CopyDescriptor = supHeapAlloc(TotalSize);
    if (CopyDescriptor == NULL)
        return STATUS_MEMORY_NOT_ALLOCATED;

    if (kdReadSystemMemoryEx(BoundaryDescriptorAddress,
        CopyDescriptor,
        TotalSize,
        NULL))
    {
        *BoundaryDescriptor = CopyDescriptor;
        if (BoundaryDescriptorSize)
            *BoundaryDescriptorSize = TotalSize;
    }
    else
        return STATUS_DEVICE_NOT_READY;

    return STATUS_SUCCESS;
}

/*
* ObpValidateSidBuffer
*
* Purpose:
*
* Check if given SID is valid.
*
*/
BOOLEAN ObpValidateSidBuffer(
    PSID Sid,
    SIZE_T BufferSize
)
{
    PUCHAR Count;

    if (BufferSize < RtlLengthRequiredSid(0))
        return FALSE;

    Count = RtlSubAuthorityCountSid(Sid);
    if (BufferSize < RtlLengthRequiredSid(*Count))
        return FALSE;

    return RtlValidSid(Sid);
}

/*
* ObEnumerateBoundaryDescriptorEntries
*
* Purpose:
*
* Walk each boundary descriptor entry, validate it and run optional callback.
*
*/
NTSTATUS ObEnumerateBoundaryDescriptorEntries(
    _In_ OBJECT_BOUNDARY_DESCRIPTOR *BoundaryDescriptor,
    _In_opt_ PENUMERATE_BOUNDARY_DESCRIPTOR_CALLBACK Callback,
    _In_opt_ PVOID Context
)
{
    ULONG EntrySize, TotalItems = 0, NameEntries = 0, IntegrityLabelEntries = 0;
    ULONG_PTR DataEnd;
    OBJECT_BOUNDARY_ENTRY *CurrentEntry, *NextEntry;

    if (BoundaryDescriptor->TotalSize < sizeof(OBJECT_BOUNDARY_DESCRIPTOR))
        return STATUS_INVALID_PARAMETER;

    if (BoundaryDescriptor->Version != 1)
        return STATUS_INVALID_PARAMETER;

    DataEnd = (ULONG_PTR)BoundaryDescriptor + BoundaryDescriptor->TotalSize;
    if (DataEnd < (ULONG_PTR)BoundaryDescriptor)
        return STATUS_INVALID_PARAMETER;

    CurrentEntry = (OBJECT_BOUNDARY_ENTRY*)((PBYTE)BoundaryDescriptor +
        sizeof(OBJECT_BOUNDARY_DESCRIPTOR));

    do {

        EntrySize = CurrentEntry->EntrySize;
        if (EntrySize < sizeof(OBJECT_BOUNDARY_ENTRY))
            return STATUS_INVALID_PARAMETER;

        TotalItems++;

        NextEntry = (OBJECT_BOUNDARY_ENTRY*)ALIGN_UP(((PBYTE)CurrentEntry + EntrySize), ULONG_PTR);

        if ((NextEntry < CurrentEntry) || ((ULONG_PTR)NextEntry > DataEnd))
            return STATUS_INVALID_PARAMETER;

        if (CurrentEntry->EntryType == OBNS_Name) {
            if (++NameEntries > 1)
                return STATUS_DUPLICATE_NAME;
        }
        else

            if (CurrentEntry->EntryType == OBNS_SID) {
                if (!ObpValidateSidBuffer(
                    (PSID)((PBYTE)CurrentEntry + sizeof(OBJECT_BOUNDARY_ENTRY)),
                    EntrySize - sizeof(OBJECT_BOUNDARY_ENTRY)))
                {
                    return STATUS_INVALID_PARAMETER;
                }
            }
            else
                if (CurrentEntry->EntryType == OBNS_IntegrityLabel) {
                    if (++IntegrityLabelEntries > 1)
                        return STATUS_DUPLICATE_OBJECTID;
                }

        if (Callback) {
            if (Callback(CurrentEntry, Context))
                return STATUS_SUCCESS;
        }

        CurrentEntry = NextEntry;

    } while ((ULONG_PTR)CurrentEntry < (ULONG_PTR)DataEnd);

    return (TotalItems != BoundaryDescriptor->Items) ? STATUS_INVALID_PARAMETER : STATUS_SUCCESS;
}

/*
* ObDumpAlpcPortObjectVersionAware
*
* Purpose:
*
* Return dumped ALPC_PORT object version aware.
*
* Use supVirtualFree to free returned buffer.
*
*/
_Success_(return != NULL)
PVOID ObDumpAlpcPortObjectVersionAware(
    _In_ ULONG_PTR ObjectAddress,
    _Out_ PULONG Size,
    _Out_ PULONG Version
)
{
    PVOID ObjectBuffer = NULL;
    ULONG ObjectSize = 0;
    ULONG ObjectVersion = 0;

    switch (g_NtBuildNumber) {
    case 7600:
    case 7601:
        ObjectSize = sizeof(ALPC_PORT_7600);
        ObjectVersion = 1;
        break;
    case 9200:
        ObjectSize = sizeof(ALPC_PORT_9200);
        ObjectVersion = 2;
        break;
    case 9600:
        ObjectSize = sizeof(ALPC_PORT_9600);
        ObjectVersion = 3;
        break;
    case 10240:
    case 10586:
    case 14393:
    case 15063:
    case 16299:
    case 17134:
    case 17763:
        ObjectSize = sizeof(ALPC_PORT_10240);
        ObjectVersion = 4;
        break;
    default:
        break;
    }

    if (ObjectSize) {
        ObjectSize = ALIGN_UP_BY(ObjectSize, PAGE_SIZE);
        ObjectBuffer = supVirtualAlloc(ObjectSize);
        if (ObjectBuffer == NULL) {
            return NULL;
        }

        if (!kdReadSystemMemory(
            ObjectAddress,
            ObjectBuffer,
            (ULONG)ObjectSize))
        {
            supVirtualFree(ObjectBuffer);
            return NULL;
        }
    }

    if (Size)
        *Size = ObjectSize;

    if (Version)
        *Version = ObjectVersion;

    return ObjectBuffer;
}

/*
* ObDumpDirectoryObjectVersionAware
*
* Purpose:
*
* Return dumped OBJECT_DIRECTORY object version aware.
*
* Use supHeapFree to free returned buffer.
*
*/
_Success_(return != NULL)
PVOID ObDumpDirectoryObjectVersionAware(
    _In_ ULONG_PTR ObjectAddress,
    _Out_ PULONG Size,
    _Out_ PULONG Version
)
{
    ULONG ObjectVersion;
    ULONG ObjectSize;
    PVOID ObjectPtr;

    switch (g_NtBuildNumber) {

    case 7600:
    case 7601:
    case 9200:
    case 9600:
        ObjectVersion = 1;
        ObjectSize = sizeof(OBJECT_DIRECTORY);
        break;

    case 10240:
    case 10586:
    case 14393:
        ObjectVersion = 2;
        ObjectSize = sizeof(OBJECT_DIRECTORY_V2);
        break;

    default:
        ObjectVersion = 3;
        ObjectSize = sizeof(OBJECT_DIRECTORY_V3);
        break;
    }

    ObjectPtr = supHeapAlloc(ObjectSize);
    if (ObjectPtr == NULL)
        return NULL;

    if (!kdReadSystemMemoryEx(
        ObjectAddress,
        ObjectPtr,
        ObjectSize,
        NULL))
    {
        supHeapFree(ObjectPtr);
        return NULL;
    }

    *Version = ObjectVersion;
    *Size = ObjectSize;

    return ObjectPtr;
}

/*
* ObDecodeTypeIndex
*
* Purpose:
*
* Decode object TypeIndex, encoding introduced in win10
*
* Limitation:
*
* Only for Win10+ use
*
*/
UCHAR ObDecodeTypeIndex(
    _In_ PVOID Object,
    _In_ UCHAR EncodedTypeIndex
)
{
    UCHAR          TypeIndex;
    POBJECT_HEADER ObjectHeader;

    if (g_kdctx.ObHeaderCookie == 0) {
        return EncodedTypeIndex;
    }

    ObjectHeader = OBJECT_TO_OBJECT_HEADER(Object);
    TypeIndex = (EncodedTypeIndex ^ (UCHAR)((ULONG_PTR)ObjectHeader >> OBJECT_SHIFT) ^ g_kdctx.ObHeaderCookie);
    return TypeIndex;
}

/*
* ObpFindHeaderCookie
*
* Purpose:
*
* Parse ObGetObjectType and extract object header cookie variable address
*
* Limitation:
*
* Only for Win10+ use
*
*/
UCHAR ObpFindHeaderCookie(
    _In_ ULONG_PTR MappedImageBase,
    _In_ ULONG_PTR KernelImageBase
)
{
    BOOL       cond = FALSE;
    UCHAR      ObHeaderCookie = 0;
    ULONG_PTR  Address = 0, KmAddress;
    UINT       c;
    LONG       rel = 0;
    hde64s     hs;

    __try {

        do {

            Address = (ULONG_PTR)GetProcAddress((PVOID)MappedImageBase, "ObGetObjectType");
            if (Address == 0) {
                break;
            }

            c = 0;
            RtlSecureZeroMemory(&hs, sizeof(hs));
            do {
                //movzx   ecx, byte ptr cs:ObHeaderCookie
                if ((*(PBYTE)(Address + c) == 0x0f) &&
                    (*(PBYTE)(Address + c + 1) == 0xb6) &&
                    (*(PBYTE)(Address + c + 2) == 0x0d))
                {
                    rel = *(PLONG)(Address + c + 3);
                    break;
                }

                hde64_disasm((void*)(Address + c), &hs);
                if (hs.flags & F_ERROR)
                    break;
                c += hs.len;

            } while (c < 256);
            KmAddress = Address + c + 7 + rel;

            KmAddress = KernelImageBase + KmAddress - MappedImageBase;

            if (!kdAddressInNtOsImage((PVOID)KmAddress))
                break;

            if (!kdReadSystemMemoryEx(
                KmAddress,
                &ObHeaderCookie,
                sizeof(ObHeaderCookie),
                NULL))
            {
                break;
            }

        } while (cond);

    }
    __except (exceptFilter(GetExceptionCode(), GetExceptionInformation())) {
        return 0;
    }

    return ObHeaderCookie;
}

/*
* ObFindObpPrivateNamespaceLookupTable2
*
* Purpose:
*
* Locate and return address of namespace table.
*
* Limitation:
*
* OS dependent, Windows 10 (14393 - 17763).
*
*/
PVOID ObFindObpPrivateNamespaceLookupTable2(
    _In_ ULONG_PTR MappedImageBase,
    _In_ ULONG_PTR KernelImageBase
)
{
    BOOL    cond = FALSE;

    PVOID   SectionBase;
    ULONG   SectionSize;

    PBYTE   Signature;
    ULONG   SignatureSize, c;

    LONG    rel = 0;

    PBYTE   ScanPtr = NULL, MatchingPattern = NULL;

    hde64s  hs;

    ESERVERSILO_GLOBALS PspHostSiloGlobals;


    do {

        //
        // Locate .text image section.
        //
        SectionBase = supLookupImageSectionByName(
            TEXT_SECTION,
            TEXT_SECTION_LEGNTH,
            (PVOID)MappedImageBase,
            &SectionSize);

        if ((SectionBase == 0) || (SectionSize == 0))
            break;

        //
        // Default code scan pattern.
        //
        MatchingPattern = LeaPattern_PNS;

        //
        // Locate starting point for search ->
        // PsGetServerSiloServiceSessionId for RS4+ and PsGetServerSiloGlobals for RS1-RS3.
        //
        if (g_NtBuildNumber >= 17134) {

            ScanPtr = (PBYTE)GetProcAddress(
                (HMODULE)MappedImageBase,
                "PsGetServerSiloServiceSessionId");
        }
        else {

            switch (g_NtBuildNumber) {

            case 14393:
                SignatureSize = sizeof(PsGetServerSiloGlobalsPattern_14393);
                Signature = PsGetServerSiloGlobalsPattern_14393;
                break;
            case 15063:
            case 16299:
                SignatureSize = sizeof(PsGetServerSiloGlobalsPattern_15064_16299);
                Signature = PsGetServerSiloGlobalsPattern_15064_16299;
                break;
            default:
                SignatureSize = 0;
                Signature = 0;
                break;
            }

            if ((SignatureSize) && (Signature)) {

                ScanPtr = supFindPattern(
                    SectionBase,
                    SectionSize,
                    Signature,
                    SignatureSize);
            }
        }

        if (ScanPtr == NULL)
            break;

        c = 0;

        //
        // Find reference to PspHostSiloGlobals in code.
        //
        RtlSecureZeroMemory(&hs, sizeof(hs));

        do {
            //lea rax, PspHostSiloGlobals
            if ((*(PBYTE)(ScanPtr + c) == MatchingPattern[0]) &&
                (*(PBYTE)(ScanPtr + c + 1) == MatchingPattern[1]) &&
                (*(PBYTE)(ScanPtr + c + 2) == MatchingPattern[2]))
            {
                rel = *(PLONG)(ScanPtr + c + 3);
                break;
            }

            hde64_disasm((void*)(ScanPtr + c), &hs);
            if (hs.flags & F_ERROR)
                break;
            c += hs.len;

        } while (c < 64);

        ScanPtr = ScanPtr + c + 7 + rel;
        ScanPtr = KernelImageBase + ScanPtr - MappedImageBase;

        if (!kdAddressInNtOsImage((PVOID)ScanPtr))
            break;

        //
        // Dump PspHostSiloGlobals.
        //
        RtlSecureZeroMemory(
            &PspHostSiloGlobals,
            sizeof(PspHostSiloGlobals));

        if (!kdReadSystemMemoryEx(
            (ULONG_PTR)ScanPtr,
            &PspHostSiloGlobals,
            sizeof(PspHostSiloGlobals),
            NULL))
        {
            break;
        }

        //
        // Return adjusted address of PrivateNamespaceLookupTable.
        //
        ScanPtr += FIELD_OFFSET(OBP_SILODRIVERSTATE, PrivateNamespaceLookupTable);

    } while (cond);

    return ScanPtr;
}

/*
* ObFindObpPrivateNamespaceLookupTable
*
* Purpose:
*
* Locate and return address of private namespace table.
*
*/
PVOID ObFindObpPrivateNamespaceLookupTable(
    _In_ ULONG_PTR MappedImageBase,
    _In_ ULONG_PTR KernelImageBase
)
{
    BOOL       cond = FALSE;
    ULONG      c;
    PBYTE      Signature, MatchingPattern;
    ULONG      SignatureSize;

    LONG       rel = 0;
    hde64s     hs;

    PBYTE      ScanPtr = NULL;
    PVOID      SectionBase;
    ULONG      SectionSize;

    if (g_NtBuildNumber > 10586)
        return ObFindObpPrivateNamespaceLookupTable2(
            MappedImageBase,
            KernelImageBase);

    do {

        //
        // Locate PAGE image section.
        //
        SectionBase = supLookupImageSectionByName(
            PAGE_SECTION,
            PAGE_SECTION_LEGNTH,
            (PVOID)MappedImageBase,
            &SectionSize);

        if ((SectionBase == 0) || (SectionSize == 0))
            break;

        switch (g_NtBuildNumber) {

        case 9200:
            Signature = NamespacePattern8;
            SignatureSize = sizeof(NamespacePattern8);
            break;

        default:
            Signature = NamespacePattern;
            SignatureSize = sizeof(NamespacePattern);
            break;
        }

        ScanPtr = supFindPattern(
            SectionBase,
            SectionSize,
            Signature,
            SignatureSize);

        if (ScanPtr == NULL)
            break;

        //
        // Lookup exact value from found pattern result.
        //
        c = 0;
        RtlSecureZeroMemory(&hs, sizeof(hs));

        //
        // Default code scan pattern.
        //
        MatchingPattern = LeaPattern_PNS;

        do {
            if ((*(PBYTE)(ScanPtr + c) == MatchingPattern[0]) &&
                (*(PBYTE)(ScanPtr + c + 1) == MatchingPattern[1]) &&
                (*(PBYTE)(ScanPtr + c + 2) == MatchingPattern[2]))
            {
                rel = *(PLONG)(ScanPtr + c + 3);
                break;
            }

            hde64_disasm((void*)(ScanPtr + c), &hs);
            if (hs.flags & F_ERROR)
                break;
            c += hs.len;

        } while (c < 128);

        ScanPtr = ScanPtr + c + 7 + rel;
        ScanPtr = KernelImageBase + ScanPtr - MappedImageBase;

        if (!kdAddressInNtOsImage((PVOID)ScanPtr))
            break;

    } while (cond);

    return (PVOID)ScanPtr;
}

/*
* kdFindKiServiceTable
*
* Purpose:
*
* Find system service table pointer from ntoskrnl image.
*
*/
VOID kdFindKiServiceTable(
    _In_ ULONG_PTR MappedImageBase,
    _In_ ULONG_PTR KernelImageBase,
    _Inout_ ULONG_PTR *KiServiceTablePtr,
    _Inout_ ULONG *KiServiceLimit
)
{
    BOOL         cond = FALSE;
    UINT         c, SignatureSize;
    LONG         rel = 0;
    ULONG        SectionSize;
    ULONG_PTR    Address = 0, SectionBase = 0;
    hde64s       hs;

    PBYTE        MatchingPattern;

    KSERVICE_TABLE_DESCRIPTOR KeSystemDescriptorTable;

    __try {

        do {
            //
            // Locate .text image section.
            //
            SectionBase = (ULONG_PTR)supLookupImageSectionByName(
                TEXT_SECTION,
                TEXT_SECTION_LEGNTH,
                (PVOID)MappedImageBase,
                &SectionSize);

            if ((SectionBase == 0) || (SectionSize == 0))
                break;

            SignatureSize = sizeof(KiSystemServiceStartPattern);
            if (SignatureSize > SectionSize)
                break;

            //
            // Find KiSystemServiceStart signature.
            //
            Address = (ULONG_PTR)supFindPattern(
                (PBYTE)SectionBase,
                SectionSize,
                (PBYTE)KiSystemServiceStartPattern,
                SignatureSize);

            if (Address == 0)
                break;

            Address += SignatureSize;

            c = 0;
            RtlSecureZeroMemory(&hs, sizeof(hs));

            MatchingPattern = LeaPattern_KiServiceStart;

            do {
                if ((*(PBYTE)(Address + c) == MatchingPattern[0]) &&
                    (*(PBYTE)(Address + c + 1) == MatchingPattern[1]) &&
                    (*(PBYTE)(Address + c + 2) == MatchingPattern[2]))
                {
                    rel = *(PLONG)(Address + c + 3);
                    break;
                }

                hde64_disasm((void*)(Address + c), &hs);
                if (hs.flags & F_ERROR)
                    break;
                c += hs.len;

            } while (c < 128);

            Address = Address + c + 7 + rel;
            Address = KernelImageBase + Address - MappedImageBase;

            if (!kdAddressInNtOsImage((PVOID)Address))
                break;

            RtlSecureZeroMemory(&KeSystemDescriptorTable,
                sizeof(KeSystemDescriptorTable));

            if (!kdReadSystemMemoryEx(
                Address,
                &KeSystemDescriptorTable,
                sizeof(KeSystemDescriptorTable),
                NULL))
            {
                break;
            }

            *KiServiceLimit = KeSystemDescriptorTable.Limit;
            *KiServiceTablePtr = KeSystemDescriptorTable.Base;

        } while (cond);

    }
    __except (exceptFilter(GetExceptionCode(), GetExceptionInformation())) {
        return;
    }
    return;
}

/*
* ObGetDirectoryObjectAddress
*
* Purpose:
*
* Obtain directory object kernel address by opening directory by name
* and quering resulted handle in NtQuerySystemInformation(SystemHandleInformation) handle dump
*
*/
BOOL ObGetDirectoryObjectAddress(
    _In_opt_ LPWSTR lpDirectory,
    _Inout_ PULONG_PTR lpRootAddress,
    _Inout_opt_ PUSHORT lpTypeIndex
)
{
    BOOL                bFound = FALSE;
    HANDLE              hDirectory = NULL;
    NTSTATUS            status;
    LPWSTR              lpTarget;
    OBJECT_ATTRIBUTES   objattr;
    UNICODE_STRING      objname;

    if (lpRootAddress == NULL)
        return bFound;

    if (lpDirectory == NULL) {
        lpTarget = L"\\";
    }
    else {
        lpTarget = lpDirectory;
    }

    RtlInitUnicodeString(&objname, lpTarget);
    InitializeObjectAttributes(&objattr, &objname, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = NtOpenDirectoryObject(
        &hDirectory,
        DIRECTORY_QUERY,
        &objattr);

    if (NT_SUCCESS(status)) {

        bFound = supQueryObjectFromHandle(
            hDirectory,
            lpRootAddress,
            lpTypeIndex);

        NtClose(hDirectory);
    }
    return bFound;
}

/*
* ObQueryNameString
*
* Purpose:
*
* Reads object name from kernel memory, returned buffer must be freed with supHeapFree
*
*/
LPWSTR ObQueryNameString(
    _In_ ULONG_PTR NameInfoAddress,
    _Out_opt_ PSIZE_T ReturnLength
)
{
    ULONG  fLen;
    LPWSTR lpObjectName = NULL;

    OBJECT_HEADER_NAME_INFO NameInfo;

    if (ReturnLength)
        *ReturnLength = 0;

    RtlSecureZeroMemory(&NameInfo, sizeof(OBJECT_HEADER_NAME_INFO));

    if (kdReadSystemMemoryEx(
        NameInfoAddress,
        &NameInfo,
        sizeof(OBJECT_HEADER_NAME_INFO),
        NULL))
    {
        if (NameInfo.Name.Length) {
            fLen = NameInfo.Name.Length + sizeof(UNICODE_NULL);
            lpObjectName = supHeapAlloc(fLen);
            if (lpObjectName != NULL) {
                NameInfoAddress = (ULONG_PTR)NameInfo.Name.Buffer;

                if (kdReadSystemMemoryEx(
                    NameInfoAddress,
                    lpObjectName,
                    NameInfo.Name.Length,
                    NULL))
                {
                    if (ReturnLength)
                        *ReturnLength = fLen;
                }
                else {
                    supHeapFree(lpObjectName);
                    lpObjectName = NULL;
                }

            }
        }
    }

    return lpObjectName;
}

/*
* ObpCopyObjectBasicInfo
*
* Purpose:
*
* Read object related data from kernel to local user copy.
* Returned object must be freed wtih supHeapFree when no longer needed.
*
* Parameters:
*
*   ObjectAddress - kernel address of object specified type (e.g. DRIVER_OBJECT).
*   ObjectHeaderAddress - OBJECT_HEADER structure kernel address.
*   ObjectHeaderAddressValid - if set then ObjectHeaderAddress in already converted form.
*   DumpedObjectHeader - pointer to OBJECT_HEADER structure previously dumped.
*
* Return Value:
*
*   Pointer to OBJINFO structure allocated from WinObjEx heap and filled with kernel data.
*
*/
POBJINFO ObpCopyObjectBasicInfo(
    _In_ ULONG_PTR ObjectAddress,
    _In_ ULONG_PTR ObjectHeaderAddress,
    _In_ BOOL ObjectHeaderAddressValid,
    _In_opt_ POBJECT_HEADER DumpedObjectHeader
)
{
    ULONG_PTR       HeaderAddress = 0, InfoHeaderAddress = 0;
    POBJINFO        lpData = NULL;
    OBJECT_HEADER   ObjectHeader, *pObjectHeader;

    //
    // Convert object address to object header address.
    //
    if (ObjectHeaderAddressValid) {
        HeaderAddress = ObjectHeaderAddress;
    }
    else {
        HeaderAddress = (ULONG_PTR)OBJECT_TO_OBJECT_HEADER(ObjectAddress);
    }

    //
    // ObjectHeader already dumped, copy it.
    //
    if (DumpedObjectHeader) {
        pObjectHeader = DumpedObjectHeader;
    }
    else {
        //
        // ObjectHeader wasn't dumped, validate it address and do dump.
        //
        if (HeaderAddress < g_kdctx.SystemRangeStart)
            return NULL;

        //
        // Read OBJECT_HEADER.
        //
        RtlSecureZeroMemory(&ObjectHeader, sizeof(OBJECT_HEADER));
        if (!kdReadSystemMemoryEx(
            HeaderAddress,
            &ObjectHeader,
            sizeof(OBJECT_HEADER),
            NULL))
        {
#ifdef _DEBUG
            OutputDebugStringA(__FUNCTION__);
            OutputDebugStringA("kdReadSystemMemoryEx(ObjectHeaderAddress) failed");
#endif

            return NULL;
        }

        pObjectHeader = &ObjectHeader;
    }

    //
    // Allocate OBJINFO structure, exit on fail.
    //
    lpData = supHeapAlloc(sizeof(OBJINFO));
    if (lpData == NULL)
        return NULL;

    lpData->ObjectAddress = ObjectAddress;
    lpData->HeaderAddress = HeaderAddress;

    //
    // Copy object header.
    //
    supCopyMemory(
        &lpData->ObjectHeader,
        sizeof(OBJECT_HEADER),
        pObjectHeader,
        sizeof(OBJECT_HEADER));

    //
    // Query and copy quota info if exist.
    //
    InfoHeaderAddress = 0;

    if (ObHeaderToNameInfoAddress(
        pObjectHeader->InfoMask,
        HeaderAddress,
        &InfoHeaderAddress,
        HeaderQuotaInfoFlag))
    {
        kdReadSystemMemoryEx(
            HeaderAddress,
            &lpData->ObjectQuotaHeader,
            sizeof(OBJECT_HEADER_QUOTA_INFO),
            NULL);
    }

    return lpData;
}

/*
* ObpWalkDirectory
*
* Purpose:
*
* Walks given directory and looks for specified object inside
* Returned object must be freed wtih supHeapFree when no longer needed.
*
* Note:
*
* OBJECT_DIRECTORY definition changed in Windows 10, however this doesn't require
* this routine change as we rely here only on HashBuckets which is on same offset.
*
*/
POBJINFO ObpWalkDirectory(
    _In_ LPWSTR lpObjectToFind,
    _In_ ULONG_PTR DirectoryAddress
)
{
    BOOL      bFound;
    INT       c;
    SIZE_T    retSize;
    LPWSTR    lpObjectName;
    ULONG_PTR ObjectHeaderAddress, item0, item1, InfoHeaderAddress;

    OBJECT_HEADER          ObjectHeader;
    OBJECT_DIRECTORY       DirObject;
    OBJECT_DIRECTORY_ENTRY Entry;

    __try {

        if (lpObjectToFind == NULL)
            return NULL;

        //
        // Read object directory at address.
        //
        RtlSecureZeroMemory(&DirObject, sizeof(OBJECT_DIRECTORY));

        if (!kdReadSystemMemoryEx(
            DirectoryAddress,
            &DirObject,
            sizeof(OBJECT_DIRECTORY),
            NULL))
        {

#ifdef _DEBUG
            OutputDebugStringA(__FUNCTION__);
            OutputDebugStringA("kdReadSystemMemoryEx(DirectoryAddress) failed");
#endif
            return NULL;
        }

        lpObjectName = NULL;
        retSize = 0;
        bFound = FALSE;

        //
        // Check if root special case.
        //
        if (_strcmpi(lpObjectToFind, L"\\") == 0) {

            return ObpCopyObjectBasicInfo(
                DirectoryAddress,
                0,
                FALSE,
                NULL);
        }

        //
        // Not a root directory, scan given object directory.
        //
        for (c = 0; c < NUMBER_HASH_BUCKETS; c++) {

            item0 = (ULONG_PTR)DirObject.HashBuckets[c];
            if (item0 != 0) {

                item1 = item0;
                do {

                    //
                    // Read object directory entry, exit on fail.
                    //
                    RtlSecureZeroMemory(&Entry, sizeof(OBJECT_DIRECTORY_ENTRY));

                    if (!kdReadSystemMemoryEx(
                        item1,
                        &Entry,
                        sizeof(OBJECT_DIRECTORY_ENTRY),
                        NULL))
                    {
#ifdef _DEBUG
                        OutputDebugStringA(__FUNCTION__);
                        OutputDebugStringA("kdReadSystemMemoryEx(OBJECT_DIRECTORY_ENTRY(HashEntry)) failed");
#endif
                        break;
                    }

                    //
                    // Read object header, skip entry on fail.
                    //
                    RtlSecureZeroMemory(&ObjectHeader, sizeof(OBJECT_HEADER));
                    ObjectHeaderAddress = (ULONG_PTR)OBJECT_TO_OBJECT_HEADER(Entry.Object);

                    if (!kdReadSystemMemoryEx(
                        ObjectHeaderAddress,
                        &ObjectHeader,
                        sizeof(OBJECT_HEADER),
                        NULL))
                    {
#ifdef _DEBUG
                        OutputDebugStringA(__FUNCTION__);
                        OutputDebugStringA("kdReadSystemMemoryEx(ObjectHeaderAddress(Entry.Object)) failed");
#endif
                        goto NextItem;
                    }

                    //
                    // Check if object has name, skip entry on fail.
                    //
                    InfoHeaderAddress = 0;
                    retSize = 0;

                    if (!ObHeaderToNameInfoAddress(
                        ObjectHeader.InfoMask,
                        ObjectHeaderAddress,
                        &InfoHeaderAddress,
                        HeaderNameInfoFlag))
                    {
                        goto NextItem;
                    }

                    //
                    // If object has name, query it.
                    //
                    lpObjectName = ObQueryNameString(InfoHeaderAddress, &retSize);
                    if ((lpObjectName != NULL) && (retSize != 0)) {

                        //
                        // Compare full object names.
                        //
                        bFound = (_strcmpi(lpObjectName, lpObjectToFind) == 0);
                        supHeapFree(lpObjectName);

                        //
                        // if they're identical, allocate item info and copy it.
                        //
                        if (bFound) {

                            return ObpCopyObjectBasicInfo(
                                (ULONG_PTR)Entry.Object,
                                ObjectHeaderAddress,
                                TRUE,
                                &ObjectHeader);

                        }
                    } //ObQueryName                 

                NextItem:
                    item1 = (ULONG_PTR)Entry.ChainLink;
                } while (item1 != 0);
            }
        }

    }
    __except (exceptFilter(GetExceptionCode(), GetExceptionInformation())) {
        return NULL;
    }
    return NULL;
}

/*
* ObQueryObjectByAddress
*
* Purpose:
*
* Look for object at specified address.
* Returned object memory must be released with supHeapFree when object is no longer needed.
*
*/
POBJINFO ObQueryObjectByAddress(
    _In_ ULONG_PTR ObjectAddress
)
{
    ULONG_PTR ObjectHeaderAddress;
    OBJECT_HEADER ObjectHeader;

    //
    // Read object header, fail is critical.
    //
    RtlSecureZeroMemory(&ObjectHeader, sizeof(OBJECT_HEADER));
    ObjectHeaderAddress = (ULONG_PTR)OBJECT_TO_OBJECT_HEADER(ObjectAddress);

    if (!kdReadSystemMemoryEx(
        ObjectHeaderAddress,
        &ObjectHeader,
        sizeof(OBJECT_HEADER),
        NULL))
    {
#ifdef _DEBUG
        OutputDebugStringA(__FUNCTION__);
        OutputDebugStringA("kdReadSystemMemoryEx(ObjectHeaderAddress(ObjectAddress)) failed");
#endif
        return NULL;
    }

    return ObpCopyObjectBasicInfo(
        (ULONG_PTR)ObjectAddress,
        ObjectHeaderAddress,
        TRUE,
        &ObjectHeader);
}

/*
* ObQueryObject
*
* Purpose:
*
* Look for object inside specified directory.
* If object is directory look for it in upper directory.
* Returned object memory must be released with supHeapFree when object is no longer needed.
*
*/
POBJINFO ObQueryObject(
    _In_ LPWSTR lpDirectory,
    _In_ LPWSTR lpObjectName
)
{
    BOOL       needFree = FALSE;
    ULONG_PTR  DirectoryAddress;
    SIZE_T     i, l, rdirLen, ldirSz;
    LPWSTR     SingleDirName, LookupDirName;

    if (
        (lpObjectName == NULL) ||
        (lpDirectory == NULL) ||
        (g_kdctx.hDevice == NULL)

        )
    {
        return NULL;
    }

    __try {

        LookupDirName = lpDirectory;

        //
        // 1) Check if object is directory self
        // Extract directory name and compare (case insensitive) with object name
        // Else go to 3
        //
        l = 0;
        rdirLen = _strlen(lpDirectory);
        for (i = 0; i < rdirLen; i++) {
            if (lpDirectory[i] == '\\')
                l = i + 1;
        }
        SingleDirName = &lpDirectory[l];
        if (_strcmpi(SingleDirName, lpObjectName) == 0) {
            //
            //  2) If we are looking for directory itself, move search directory up
            //  e.g. lpDirectory = \ObjectTypes, lpObjectName = ObjectTypes then lpDirectory = \ 
            //
            ldirSz = rdirLen * sizeof(WCHAR) + sizeof(UNICODE_NULL);
            LookupDirName = supHeapAlloc(ldirSz);
            if (LookupDirName == NULL)
                return NULL;

            needFree = TRUE;

            //special case for root 
            if (l == 1) l++;

            supCopyMemory(LookupDirName, ldirSz, lpDirectory, (l - 1) * sizeof(WCHAR));
        }

        //
        // 3) Get Directory address where we will look for object
        //
        DirectoryAddress = 0;
        if (ObGetDirectoryObjectAddress(LookupDirName, &DirectoryAddress, NULL)) {

            if (needFree)
                supHeapFree(LookupDirName);

            //
            // 4) Find object in directory by name (case insensitive)
            //
            return ObpWalkDirectory(lpObjectName, DirectoryAddress);

        }
    }

    __except (exceptFilter(GetExceptionCode(), GetExceptionInformation())) {
        return NULL;
    }
    return NULL;
}

/*
* ObDumpTypeInfo
*
* Purpose:
*
* Dumps Type header including initializer.
*
*/
BOOL ObDumpTypeInfo(
    _In_ ULONG_PTR ObjectAddress,
    _Inout_ POBJECT_TYPE_COMPATIBLE ObjectTypeInfo
)
{
    return kdReadSystemMemoryEx(
        ObjectAddress,
        ObjectTypeInfo,
        sizeof(OBJECT_TYPE_COMPATIBLE),
        NULL);
}

/*
* ObpWalkDirectoryRecursive
*
* Purpose:
*
* Recursively dump Object Manager directories.
*
* Note:
*
* OBJECT_DIRECTORY definition changed in Windows 10, however this doesn't require
* this routine change as we rely here only on HashBuckets which is on same offset.
*
*/
VOID ObpWalkDirectoryRecursive(
    _In_ BOOL fIsRoot,
    _In_ PLIST_ENTRY ListHead,
    _In_ HANDLE ListHeap,
    _In_opt_ LPWSTR lpRootDirectory,
    _In_ ULONG_PTR DirectoryAddress,
    _In_ USHORT DirectoryTypeIndex
)
{
    UCHAR      ObjectTypeIndex;
    INT        c;
    SIZE_T     dirLen, fLen, rdirLen, retSize;
    ULONG_PTR  ObjectHeaderAddress, item0, item1, InfoHeaderAddress;
    POBJREF    ObjectEntry;
    LPWSTR     lpObjectName, lpDirectoryName;

    OBJECT_HEADER ObjectHeader;
    OBJECT_DIRECTORY DirObject;
    OBJECT_DIRECTORY_ENTRY Entry;

    RtlSecureZeroMemory(&DirObject, sizeof(OBJECT_DIRECTORY));
    if (!kdReadSystemMemoryEx(
        DirectoryAddress,
        &DirObject,
        sizeof(OBJECT_DIRECTORY),
        NULL))
    {
#ifdef _DEBUG
        OutputDebugStringA(__FUNCTION__);
        OutputDebugStringA("kdReadSystemMemoryEx(DirectoryAddress) failed");
#endif
        return;
    }

    if (lpRootDirectory != NULL) {
        rdirLen = (1 + _strlen(lpRootDirectory)) * sizeof(WCHAR);
    }
    else {
        rdirLen = 0;
    }

    lpObjectName = NULL;
    retSize = 0;
    ObjectTypeIndex = 0;

    for (c = 0; c < NUMBER_HASH_BUCKETS; c++) {

        item0 = (ULONG_PTR)DirObject.HashBuckets[c];
        if (item0 != 0) {
            item1 = item0;
            do {

                //
                // Read object directory entry.
                //
                RtlSecureZeroMemory(&Entry, sizeof(OBJECT_DIRECTORY_ENTRY));
                if (kdReadSystemMemoryEx(
                    item1,
                    &Entry,
                    sizeof(OBJECT_DIRECTORY_ENTRY),
                    NULL))
                {

                    //
                    // Read object.
                    // First read header from directory entry object.
                    //
                    RtlSecureZeroMemory(&ObjectHeader, sizeof(OBJECT_HEADER));
                    ObjectHeaderAddress = (ULONG_PTR)OBJECT_TO_OBJECT_HEADER(Entry.Object);
                    if (kdReadSystemMemoryEx(
                        ObjectHeaderAddress,
                        &ObjectHeader,
                        sizeof(OBJECT_HEADER),
                        NULL))
                    {

                        //
                        // Second read object data.
                        // Query object name.
                        //
                        InfoHeaderAddress = 0;
                        retSize = 0;
                        if (ObHeaderToNameInfoAddress(
                            ObjectHeader.InfoMask,
                            ObjectHeaderAddress,
                            &InfoHeaderAddress,
                            HeaderNameInfoFlag))
                        {
                            lpObjectName = ObQueryNameString(
                                InfoHeaderAddress,
                                &retSize);
                        }

                        //
                        // Allocate object entry.
                        //
                        ObjectEntry = RtlAllocateHeap(
                            ListHeap,
                            HEAP_ZERO_MEMORY,
                            sizeof(OBJREF));

                        if (ObjectEntry) {

                            //
                            // Save object address.
                            //
                            ObjectEntry->ObjectAddress = (ULONG_PTR)Entry.Object;
                            ObjectEntry->HeaderAddress = ObjectHeaderAddress;
                            ObjectEntry->TypeIndex = ObjectHeader.TypeIndex;

                            //
                            // Copy dir + name.
                            //
                            if (lpObjectName) {

                                fLen = (_strlen(lpObjectName) * sizeof(WCHAR)) +
                                    (2 * sizeof(WCHAR)) +
                                    rdirLen + sizeof(UNICODE_NULL);

                                ObjectEntry->ObjectName = RtlAllocateHeap(
                                    ListHeap,
                                    HEAP_ZERO_MEMORY,
                                    fLen);

                                if (ObjectEntry->ObjectName) {
                                    _strcpy(ObjectEntry->ObjectName, lpRootDirectory);
                                    if (fIsRoot == FALSE) {
                                        _strcat(ObjectEntry->ObjectName, L"\\");
                                    }
                                    _strcat(ObjectEntry->ObjectName, lpObjectName);
                                }
                            }

                            InsertHeadList(ListHead, &ObjectEntry->ListEntry);
                        }

                        //
                        // Check if current object is a directory.
                        //
                        ObjectTypeIndex = ObDecodeTypeIndex(Entry.Object, ObjectHeader.TypeIndex);
                        if (ObjectTypeIndex == DirectoryTypeIndex) {

                            //
                            // Build new directory string (old directory + \ + current).
                            //
                            fLen = 0;
                            if (lpObjectName) {
                                fLen = retSize;
                            }

                            dirLen = fLen + rdirLen + (2 * sizeof(WCHAR)) + sizeof(UNICODE_NULL);
                            lpDirectoryName = supHeapAlloc(dirLen);
                            if (lpDirectoryName) {
                                _strcpy(lpDirectoryName, lpRootDirectory);
                                if (fIsRoot == FALSE) {
                                    _strcat(lpDirectoryName, L"\\");
                                }
                                if (lpObjectName) {
                                    _strcat(lpDirectoryName, lpObjectName);
                                }
                            }

                            //
                            // Walk subdirectory.
                            //
                            ObpWalkDirectoryRecursive(
                                FALSE,
                                ListHead,
                                ListHeap,
                                lpDirectoryName,
                                (ULONG_PTR)Entry.Object,
                                DirectoryTypeIndex);

                            if (lpDirectoryName) {
                                supHeapFree(lpDirectoryName);
                                lpDirectoryName = NULL;
                            }
                        }

                        if (lpObjectName) {
                            supHeapFree(lpObjectName);
                            lpObjectName = NULL;
                        }

                    } //if (kdReadSystemMemoryEx(OBJECT_HEADER)

                    item1 = (ULONG_PTR)Entry.ChainLink;

                } //if (kdReadSystemMemoryEx(OBJECT_DIRECTORY_ENTRY)			
                else {
                    item1 = 0;
                }
            } while (item1 != 0);
        }
    }
}

/*
* ObpWalkPrivateNamespaceTable
*
* Purpose:
*
* Dump Object Manager private namespace objects.
*
* Note:
*
* OBJECT_DIRECTORY definition changed in Windows 10, however this doesn't require
* this routine change as we rely here only on HashBuckets which is on same offset.
*
*/
BOOL ObpWalkPrivateNamespaceTable(
    _In_ PLIST_ENTRY ListHead,
    _In_ HANDLE ListHeap,
    _In_ ULONG_PTR TableAddress
)
{
    ULONG         i, j, c = 0;
    SIZE_T        retSize = 0;
    ULONG_PTR     ObjectHeaderAddress, item0, item1, InfoHeaderAddress;
    PLIST_ENTRY   Next, Head;
    LIST_ENTRY    ListEntry;
    POBJREF       ObjectEntry;
    LPWSTR        lpObjectName = NULL;

    OBJECT_HEADER                ObjectHeader;
    OBJECT_DIRECTORY             DirObject;
    OBJECT_DIRECTORY_ENTRY       Entry;
    OBJECT_NAMESPACE_LOOKUPTABLE LookupTable;
    OBJECT_NAMESPACE_ENTRY       LookupEntry;

    if (
        (ListHead == NULL) ||
        (TableAddress == 0)
        )
    {
        return FALSE;
    }

    //
    // Dump namespace lookup table.
    //
    RtlSecureZeroMemory(&LookupTable, sizeof(OBJECT_NAMESPACE_LOOKUPTABLE));
    if (!kdReadSystemMemoryEx(
        TableAddress,
        &LookupTable,
        sizeof(OBJECT_NAMESPACE_LOOKUPTABLE),
        NULL))
    {
        return FALSE;
    }

    for (i = 0; i < NUMBER_HASH_BUCKETS; i++) {

        ListEntry = LookupTable.HashBuckets[i];

        Head = (PLIST_ENTRY)(TableAddress + (i * sizeof(LIST_ENTRY)));
        Next = ListEntry.Flink;

        while (Next != Head) {

            RtlSecureZeroMemory(&LookupEntry, sizeof(OBJECT_NAMESPACE_ENTRY));
            if (!kdReadSystemMemoryEx(
                (ULONG_PTR)Next,
                &LookupEntry,
                sizeof(OBJECT_NAMESPACE_ENTRY),
                NULL))
            {
                break;
            }

            ListEntry = LookupEntry.ListEntry;

            RtlSecureZeroMemory(&DirObject, sizeof(OBJECT_DIRECTORY));
            if (!kdReadSystemMemoryEx(
                (ULONG_PTR)LookupEntry.NamespaceRootDirectory,
                &DirObject,
                sizeof(OBJECT_DIRECTORY),
                NULL))
            {
                break;
            }

            for (j = 0; j < NUMBER_HASH_BUCKETS; j++) {
                item0 = (ULONG_PTR)DirObject.HashBuckets[j];
                if (item0 != 0) {
                    item1 = item0;
                    do {

                        RtlSecureZeroMemory(&Entry, sizeof(OBJECT_DIRECTORY_ENTRY));

                        if (kdReadSystemMemoryEx(
                            item1,
                            &Entry,
                            sizeof(OBJECT_DIRECTORY_ENTRY),
                            NULL)) {

                            RtlSecureZeroMemory(&ObjectHeader, sizeof(OBJECT_HEADER));
                            ObjectHeaderAddress = (ULONG_PTR)OBJECT_TO_OBJECT_HEADER(Entry.Object);
                            if (kdReadSystemMemoryEx(
                                ObjectHeaderAddress,
                                &ObjectHeader,
                                sizeof(OBJECT_HEADER),
                                NULL))
                            {
                                //
                                // Allocate object entry
                                //
                                ObjectEntry = RtlAllocateHeap(
                                    ListHeap,
                                    HEAP_ZERO_MEMORY,
                                    sizeof(OBJREF));

                                if (ObjectEntry) {

                                    //
                                    // Save object address, header and type index.
                                    //
                                    ObjectEntry->ObjectAddress = (ULONG_PTR)Entry.Object;
                                    ObjectEntry->HeaderAddress = ObjectHeaderAddress;
                                    ObjectEntry->TypeIndex = ObjectHeader.TypeIndex; //save index as is (decoded if needed later)

                                    //
                                    // Save object namespace/lookup entry address.
                                    //
                                    ObjectEntry->PrivateNamespace.NamespaceDirectoryAddress = (ULONG_PTR)LookupEntry.NamespaceRootDirectory;
                                    ObjectEntry->PrivateNamespace.NamespaceLookupEntry = (ULONG_PTR)Next;
                                    ObjectEntry->PrivateNamespace.SizeOfBoundaryInformation = LookupEntry.SizeOfBoundaryInformation;

                                    //
                                    // Query object name.
                                    //
                                    InfoHeaderAddress = 0;
                                    retSize = 0;
                                    if (ObHeaderToNameInfoAddress(
                                        ObjectHeader.InfoMask,
                                        ObjectHeaderAddress,
                                        &InfoHeaderAddress,
                                        HeaderNameInfoFlag))
                                    {
                                        lpObjectName = ObQueryNameString(InfoHeaderAddress, &retSize);
                                    }

                                    //
                                    // Copy object name.
                                    //
                                    if (lpObjectName) {

                                        ObjectEntry->ObjectName = RtlAllocateHeap(
                                            ListHeap,
                                            HEAP_ZERO_MEMORY,
                                            retSize);

                                        if (ObjectEntry->ObjectName) {
                                            _strcpy(ObjectEntry->ObjectName, lpObjectName);
                                        }

                                        //
                                        // Free memory allocated for object name.
                                        //
                                        supHeapFree(lpObjectName);
                                        lpObjectName = NULL;
                                    }
                                    c++;
                                    InsertHeadList(ListHead, &ObjectEntry->ListEntry);

                                } //if (ObjectEntry)
                            }
                            item1 = (ULONG_PTR)Entry.ChainLink;
                        }
                        else {
                            item1 = 0;
                        }
                    } while (item1 != 0);
                }
            }

            Next = ListEntry.Flink;
        }
    }
    return (c > 0);
}

/*
* ObCollectionCreate
*
* Purpose:
*
* Create collection of object directory dumped info
*
* Collection must be destroyed with ObCollectionDestroy after use.
*
* If specified will dump private namespace objects.
*
*/
BOOL ObCollectionCreate(
    _In_ POBJECT_COLLECTION Collection,
    _In_ BOOL fNamespace,
    _In_ BOOL Locked
)
{
    BOOL bResult;

    bResult = FALSE;
    if (g_kdctx.hDevice == NULL) {
        return bResult;
    }

    if (Collection == NULL) {
        return bResult;
    }

    if (!Locked) {
        RtlEnterCriticalSection(&g_kdctx.ListLock);
    }

    Collection->Heap = RtlCreateHeap(HEAP_GROWABLE, NULL, 0, 0, NULL, NULL);

    if (Collection->Heap == NULL)
        goto _FailWeLeave;

    RtlSetHeapInformation(Collection->Heap, HeapEnableTerminationOnCorruption, NULL, 0);

    __try {

        InitializeListHead(&Collection->ListHead);

        if (fNamespace == FALSE) {
            if (
                (g_kdctx.DirectoryRootAddress == 0) ||
                (g_kdctx.DirectoryTypeIndex == 0)
                )
            {
                if (!ObGetDirectoryObjectAddress(
                    NULL,
                    &g_kdctx.DirectoryRootAddress,
                    &g_kdctx.DirectoryTypeIndex))
                {
                    SetLastError(ERROR_INTERNAL_ERROR);
                    goto _FailWeLeave;
                }
            }

            if (
                (g_kdctx.DirectoryRootAddress != 0) &&
                (g_kdctx.DirectoryTypeIndex != 0)
                )
            {
                ObpWalkDirectoryRecursive(
                    TRUE,
                    &Collection->ListHead,
                    Collection->Heap,
                    L"\\",
                    g_kdctx.DirectoryRootAddress,
                    g_kdctx.DirectoryTypeIndex);

                bResult = TRUE;
            }
        }
        else {

            if (g_kdctx.ObpPrivateNamespaceLookupTable != 0) {

                bResult = ObpWalkPrivateNamespaceTable(
                    &Collection->ListHead,
                    Collection->Heap,
                    (ULONG_PTR)g_kdctx.ObpPrivateNamespaceLookupTable);
            }
            else {
                SetLastError(ERROR_INTERNAL_ERROR);
            }
        }

    }
    __except (exceptFilter(GetExceptionCode(), GetExceptionInformation())) {
        bResult = FALSE;
    }

_FailWeLeave:
    if (!Locked) {
        RtlLeaveCriticalSection(&g_kdctx.ListLock);
    }

    return bResult;
}

/*
* ObCollectionDestroy
*
* Purpose:
*
* Destroy collection with object directory dumped info
*
*/
VOID ObCollectionDestroy(
    _In_ POBJECT_COLLECTION Collection
)
{
    if (Collection == NULL)
        return;

    RtlEnterCriticalSection(&g_kdctx.ListLock);

    if (Collection->Heap) {
        RtlDestroyHeap(Collection->Heap);
        Collection->Heap = NULL;
    }
    InitializeListHead(&Collection->ListHead);

    RtlLeaveCriticalSection(&g_kdctx.ListLock);
}

/*
* ObCollectionEnumerate
*
* Purpose:
*
* Enumerate object collection and callback on each element.
*
*/
BOOL ObCollectionEnumerate(
    _In_ POBJECT_COLLECTION Collection,
    _In_ PENUMERATE_COLLECTION_CALLBACK Callback,
    _In_ PVOID Context
)
{
    BOOL        bCancelled = FALSE;
    POBJREF     ObjectEntry = NULL;
    PLIST_ENTRY Head, Next;

    if ((Collection == NULL) || (Callback == NULL))
        return FALSE;

    RtlEnterCriticalSection(&g_kdctx.ListLock);

    if (!IsListEmpty(&Collection->ListHead)) {
        Head = &Collection->ListHead;
        Next = Head->Flink;
        while ((Next != NULL) && (Next != Head)) {
            ObjectEntry = CONTAINING_RECORD(Next, OBJREF, ListEntry);
            bCancelled = Callback(ObjectEntry, Context);
            if (bCancelled)
                break;

            Next = Next->Flink;
        }
    }

    RtlLeaveCriticalSection(&g_kdctx.ListLock);

    return (bCancelled == FALSE);
}

/*
* ObCollectionFindByAddress
*
* Purpose:
*
* Find object by address in object directory dump collection.
* Do not free returned buffer, it is released in ObCollectionDestroy.
*
*/
POBJREF ObCollectionFindByAddress(
    _In_ POBJECT_COLLECTION Collection,
    _In_ ULONG_PTR ObjectAddress,
    _In_ BOOLEAN fNamespace
)
{
    BOOL        bFound = FALSE, bCollectionPresent = FALSE;
    POBJREF     ObjectEntry = NULL;
    PLIST_ENTRY Head, Next;

    if (Collection == NULL)
        return NULL;

    RtlEnterCriticalSection(&g_kdctx.ListLock);

    if (IsListEmpty(&Collection->ListHead)) {
        bCollectionPresent = ObCollectionCreate(Collection, fNamespace, TRUE);
    }
    else {
        bCollectionPresent = TRUE;
    }

    if (bCollectionPresent) {
        Head = &Collection->ListHead;
        Next = Head->Flink;
        while ((Next != NULL) && (Next != Head)) {
            ObjectEntry = CONTAINING_RECORD(Next, OBJREF, ListEntry);
            if (ObjectEntry->ObjectAddress == ObjectAddress) {
                bFound = TRUE;
                break;
            }
            Next = Next->Flink;
        }
    }

    RtlLeaveCriticalSection(&g_kdctx.ListLock);

    return (bFound) ? ObjectEntry : NULL;
}

/*
* kdQueryIopInvalidDeviceRequest
*
* Purpose:
*
* Find IopInvalidDeviceRequest assuming Windows assigned it to WinDBG driver.
*
* Kldbgdrv only defined:
*    IRP_MJ_CREATE
*    IRP_MJ_CLOSE
*    IRP_MJ_DEVICE_CONTROL
*/
PVOID kdQueryIopInvalidDeviceRequest(
    VOID
)
{
    PVOID           pHandler;
    POBJINFO        pSelfObj;
    ULONG_PTR       drvObjectAddress;
    DRIVER_OBJECT   drvObject;

    pHandler = NULL;
    pSelfObj = ObQueryObject(L"\\Driver", KLDBGDRV);
    if (pSelfObj) {

        drvObjectAddress = pSelfObj->ObjectAddress;

        RtlSecureZeroMemory(&drvObject, sizeof(drvObject));
        if (kdReadSystemMemoryEx(
            drvObjectAddress,
            &drvObject,
            sizeof(drvObject),
            NULL))
        {
            pHandler = drvObject.MajorFunction[IRP_MJ_CREATE_MAILSLOT];

            //
            // IopInvalidDeviceRequest is a routine inside ntoskrnl.
            //
            if (!kdAddressInNtOsImage(pHandler))
                pHandler = NULL;
        }
        supHeapFree(pSelfObj);
    }
    return pHandler;
}

/*
* kdIsDebugBoot
*
* Purpose:
*
* Perform check is the current OS booted with DEBUG flag
*
*/
BOOL kdIsDebugBoot(
    VOID
)
{
    ULONG rl = 0;
    SYSTEM_KERNEL_DEBUGGER_INFORMATION kdInfo;

    RtlSecureZeroMemory(&kdInfo, sizeof(kdInfo));
    NtQuerySystemInformation(SystemKernelDebuggerInformation, &kdInfo, sizeof(kdInfo), &rl);
    return kdInfo.KernelDebuggerEnabled;
}

/*
* kdReadSystemMemoryEx
*
* Purpose:
*
* Wrapper around SysDbgReadVirtual request to the KLDBGDRV
*
*/
_Success_(return == TRUE)
BOOL kdReadSystemMemoryEx(
    _In_ ULONG_PTR Address,
    _Inout_ PVOID Buffer,
    _In_ ULONG BufferSize,
    _Out_opt_ PULONG NumberOfBytesRead
)
{
    BOOL           bResult = FALSE;
    DWORD          bytesIO = 0;
    KLDBG          kldbg;
    SYSDBG_VIRTUAL dbgRequest;

    if (g_kdctx.hDevice == NULL)
        return FALSE;

    if ((Buffer == NULL) || (BufferSize == 0))
        return FALSE;

    if (Address < g_kdctx.SystemRangeStart)
        return FALSE;

    // fill parameters for KdSystemDebugControl
    dbgRequest.Address = (PVOID)Address;
    dbgRequest.Buffer = Buffer;
    dbgRequest.Request = BufferSize;

    // fill parameters for kldbgdrv ioctl
    kldbg.SysDbgRequest = SysDbgReadVirtual;
    kldbg.OutputBuffer = &dbgRequest;
    kldbg.OutputBufferSize = sizeof(SYSDBG_VIRTUAL);

    bResult = DeviceIoControl(
        g_kdctx.hDevice,
        IOCTL_KD_PASS_THROUGH,
        &kldbg,
        sizeof(kldbg),
        &dbgRequest,
        sizeof(dbgRequest),
        &bytesIO,
        NULL);

    if (NumberOfBytesRead)
        *NumberOfBytesRead = bytesIO;

    return bResult;
}

/*
* kdReadSystemMemory
*
* Purpose:
*
* Wrapper around kdReadSystemMemoryEx
*
*/
BOOL kdReadSystemMemory(
    _In_ ULONG_PTR Address,
    _Inout_ PVOID Buffer,
    _In_ ULONG BufferSize
)
{
    return kdReadSystemMemoryEx(
        Address,
        Buffer,
        BufferSize,
        NULL);
}

/*
* kdInit
*
* Purpose:
*
* Extract KLDBGDRV from application resource
*
*/
BOOL kdExtractDriver(
    _In_ LPCWSTR lpExtractTo,
    _In_ LPCWSTR lpName,
    _In_ LPCWSTR lpType
)
{
    HRSRC   hResInfo = NULL;
    HGLOBAL hResData = NULL;
    PVOID   pData;
    BOOL    bResult = FALSE;
    DWORD   dwSize = 0;
    HANDLE  hFile = INVALID_HANDLE_VALUE;

    hResInfo = FindResource(g_WinObj.hInstance, lpName, lpType);
    if (hResInfo == NULL) return bResult;

    dwSize = SizeofResource(g_WinObj.hInstance, hResInfo);
    if (dwSize == 0) return bResult;

    hResData = LoadResource(g_WinObj.hInstance, hResInfo);
    if (hResData == NULL) return bResult;

    pData = LockResource(hResData);
    if (pData) {

        hFile = CreateFile(
            lpExtractTo,
            GENERIC_WRITE,
            0,
            NULL,
            CREATE_ALWAYS,
            0,
            NULL);

        if (hFile != INVALID_HANDLE_VALUE) {
            bResult = WriteFile(hFile, pData, dwSize, &dwSize, NULL);
            CloseHandle(hFile);
        }
    }
    return bResult;
}

/*
* kdQuerySystemInformation
*
* Purpose:
*
* Query required system information and offsets.
*
*/
DWORD WINAPI kdQuerySystemInformation(
    _In_ PVOID lpParameter
)
{
    BOOL                    cond = FALSE, bResult = FALSE;
    PKLDBGCONTEXT           Context = (PKLDBGCONTEXT)lpParameter;
    PVOID                   MappedKernel = NULL;
    PRTL_PROCESS_MODULES    miSpace = NULL;
    WCHAR                   NtOskrnlFullPathName[MAX_PATH * 2];

    do {

        miSpace = supGetSystemInfo(SystemModuleInformation);
        if (miSpace == NULL)
            break;

        if (miSpace->NumberOfModules == 0)
            break;

        Context->NtOsBase = miSpace->Modules[0].ImageBase; //loaded kernel base
        Context->NtOsSize = miSpace->Modules[0].ImageSize; //loaded kernel size

        _strcpy(NtOskrnlFullPathName, g_WinObj.szSystemDirectory);
        _strcat(NtOskrnlFullPathName, TEXT("\\"));

        MultiByteToWideChar(
            CP_ACP,
            0,
            (LPCSTR)&miSpace->Modules[0].FullPathName[miSpace->Modules[0].OffsetToFileName],
            -1,
            _strend(NtOskrnlFullPathName),
            MAX_PATH);

        supHeapFree(miSpace);
        miSpace = NULL;

        MappedKernel = LoadLibraryEx(
            NtOskrnlFullPathName,
            NULL,
            DONT_RESOLVE_DLL_REFERENCES);

        if (MappedKernel == NULL)
            break;

        //
        // Locate and remember ObHeaderCookie.
        //
        if (g_WinObj.osver.dwMajorVersion >= 10) {

            Context->ObHeaderCookie = ObpFindHeaderCookie(
                (ULONG_PTR)MappedKernel,
                (ULONG_PTR)Context->NtOsBase);

        }

        //
        // Find KiServiceTable.
        //
        kdFindKiServiceTable(
            (ULONG_PTR)MappedKernel,
            (ULONG_PTR)Context->NtOsBase,
            &Context->KiServiceTableAddress,
            &Context->KiServiceLimit);

        //
        // Find namespace table.
        //
        Context->ObpPrivateNamespaceLookupTable = ObFindObpPrivateNamespaceLookupTable(
            (ULONG_PTR)MappedKernel,
            (ULONG_PTR)Context->NtOsBase);

        bResult = TRUE;

    } while (cond);

    if (MappedKernel != NULL) {
        FreeLibrary(MappedKernel);
    }
    if (miSpace != NULL) {
        supHeapFree(miSpace);
    }

    return bResult;
}

/*
* kdAddressInNtOsImage
*
* Purpose:
*
* Test if given address in range of ntoskrnl.
*
*/
BOOL __forceinline kdAddressInNtOsImage(
    _In_ PVOID Address
)
{
    return IN_REGION(Address,
        g_kdctx.NtOsBase,
        g_kdctx.NtOsSize);
}


/*
* kdInit
*
* Purpose:
*
* Enable Debug Privilege and open/load KLDBGDRV driver
*
*/
VOID kdInit(
    _In_ BOOL IsFullAdmin
)
{
    WCHAR szDrvPath[MAX_PATH * 2];

    RtlSecureZeroMemory(&g_kdctx, sizeof(g_kdctx));

    InitializeListHead(&g_kdctx.ObCollection.ListHead);

    //
    // Minimum supported client is windows 7
    // Query system range start value and if version below Win7 - leave
    //
    if (
        (g_WinObj.osver.dwMajorVersion < 6) || //any lower other vista
        ((g_WinObj.osver.dwMajorVersion == 6) && (g_WinObj.osver.dwMinorVersion == 0))//vista
        )
    {
        return;
    }

    //
    // Query "\\" directory address and remember directory object type index.
    //
    ObGetDirectoryObjectAddress(
        NULL,
        &g_kdctx.DirectoryRootAddress,
        &g_kdctx.DirectoryTypeIndex);

    //
    // Remember system range start value.
    //
    g_kdctx.SystemRangeStart = supQuerySystemRangeStart();
    if (g_kdctx.SystemRangeStart == 0) {
        if (g_NtBuildNumber < 9200) {
            g_kdctx.SystemRangeStart = MM_SYSTEM_RANGE_START_7;
        }
        else {
            g_kdctx.SystemRangeStart = MM_SYSTEM_RANGE_START_8;
        }
    }

    //
    // No admin rights, leave.
    //
    if (IsFullAdmin == FALSE)
        return;

    //
    // wodbgdrv does not need DEBUG mode.
    //

#ifndef _USE_OWN_DRIVER
    //
    // Check if system booted in the debug mode.
    //
    if (kdIsDebugBoot() == FALSE)
        return;
#endif

    //
    // Test privilege assigned and continue to load/open kldbg driver.
    //
    if (supEnablePrivilege(SE_DEBUG_PRIVILEGE, TRUE)) {

        //
        // Try to open existing device.
        //
        if (scmOpenDevice(KLDBGDRV, &g_kdctx.hDevice) == FALSE) {

            //
            // No such device exist, construct filepath and check if driver already present.
            //
            RtlSecureZeroMemory(szDrvPath, sizeof(szDrvPath));
            _strcpy(szDrvPath, g_WinObj.szSystemDirectory);
            _strcat(szDrvPath, KLDBGDRVSYS);

            //
            // If no file exists, extract it to the drivers directory.
            //
            if (!PathFileExists(szDrvPath)) {
                kdExtractDriver(szDrvPath, MAKEINTRESOURCE(IDR_KDBGDRV), L"SYS");
            }

            //
            // Load service driver and open handle for it.
            //
            g_kdctx.IsOurLoad = scmLoadDeviceDriver(KLDBGDRV, szDrvPath, &g_kdctx.hDevice);
        }

    }

    //
    // Query global variable and dump object directory if driver support available.
    //
    if (g_kdctx.hDevice != NULL) {

        ObpInitInfoBlockOffsets();

        RtlInitializeCriticalSection(&g_kdctx.ListLock);

        g_kdctx.hThreadWorker = CreateThread(
            NULL,
            0,
            kdQuerySystemInformation,
            &g_kdctx,
            0,
            NULL);
    }
}

/*
* kdShutdown
*
* Purpose:
*
* Close handle to the driver and unload it if it was loaded by our program,
* destroy object list, delete list lock.
* This routine called once, during program shutdown.
*
*/
VOID kdShutdown(
    VOID
)
{
    WCHAR szDrvPath[MAX_PATH * 2];

    if (g_kdctx.hDevice == NULL)
        return;
    //
    // If there is a valid ThreadWorker, wait for it shutdown a bit.
    //
    if (g_kdctx.hThreadWorker) {
        //
        // On wait timeout terminate worker.
        //
        if (WaitForSingleObject(g_kdctx.hThreadWorker, 5000) == WAIT_TIMEOUT)
            TerminateThread(g_kdctx.hThreadWorker, 0);
        CloseHandle(g_kdctx.hThreadWorker);
        g_kdctx.hThreadWorker = NULL;
    }

    CloseHandle(g_kdctx.hDevice);
    g_kdctx.hDevice = NULL;

    ObCollectionDestroy(&g_kdctx.ObCollection);
    RtlDeleteCriticalSection(&g_kdctx.ListLock);

    //
    // Driver was loaded, unload it.
    // Windbg recreates service and drops file everytime when kernel debug starts.
    //
    if (g_kdctx.IsOurLoad) {
        scmUnloadDeviceDriver(KLDBGDRV);

        //
        // Driver file is no longer needed.
        //
        RtlSecureZeroMemory(&szDrvPath, sizeof(szDrvPath));
        _strcpy(szDrvPath, g_WinObj.szSystemDirectory);
        _strcat(szDrvPath, KLDBGDRVSYS);
        DeleteFile(szDrvPath);
    }
}
