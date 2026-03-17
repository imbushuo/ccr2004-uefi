/** @file
  MikroTik YAFFS2 read-only filesystem driver header.

  Defines data structures for the YAFFS2 filesystem driver that reads
  MikroTik's modified YAFFS2 format from NAND flash.

  Copyright (c) 2024, MikroTik. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef MT_YAFFS2_DXE_H_
#define MT_YAFFS2_DXE_H_

#include <Uefi.h>
#include <Protocol/MikroTikNandFlash.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>
#include <Guid/FileSystemVolumeLabelInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

//
// YAFFS2 page layout: last 16 bytes are inline tags
//
#define YAFFS2_TAG_SIZE        16
#define YAFFS2_DATA_PER_PAGE   2032    // PageSize - TAG_SIZE

//
// Sequence number validity range
//
#define YAFFS2_SEQ_MIN         0x00001000U
#define YAFFS2_SEQ_MAX         0xEFFFFF00U

//
// Object types (stored in object header at offset 0)
//
#define YAFFS2_TYPE_FILE       1
#define YAFFS2_TYPE_SYMLINK    2
#define YAFFS2_TYPE_DIR        3
#define YAFFS2_TYPE_HARDLINK   4
#define YAFFS2_TYPE_SPECIAL    5

//
// Signatures
//
#define YAFFS2_VOLUME_SIGNATURE  SIGNATURE_32 ('Y', 'F', 'V', 'L')
#define YAFFS2_IFILE_SIGNATURE   SIGNATURE_32 ('Y', 'F', 'I', 'F')

//
// Parsed tags from the last 16 bytes of a NAND page
//
typedef struct {
  UINT32   SeqNumber;
  UINT32   ObjId;
  UINT32   ChunkId;     // 0 for header pages, 1+ for data pages
  UINT32   NBytes;      // For data: valid bytes in chunk; for header: file_size
  BOOLEAN  IsHeader;
} YAFFS2_TAGS;

//
// Data chunk location (tracks the winning version for each chunk_id)
//
typedef struct {
  UINT32  PageIndex;     // 0xFFFFFFFF = unset/missing
  UINT32  NBytes;        // Valid data bytes in this chunk
  UINT32  SeqNumber;     // For version comparison during scan
} YAFFS2_CHUNK_LOC;

#define YAFFS2_CHUNK_UNSET  0xFFFFFFFF

//
// Forward declaration
//
typedef struct _YAFFS2_VOLUME YAFFS2_VOLUME;

//
// Filesystem object (file, directory, symlink, hardlink)
//
typedef struct _YAFFS2_OBJECT {
  UINT32              ObjId;
  UINT32              ParentId;
  UINT32              Type;          // YAFFS2_TYPE_*
  CHAR8               Name[256];     // Null-terminated ASCII
  UINT32              Mode;          // Unix mode bits
  UINT32              MTime;         // Unix timestamp
  UINT64              FileSize;      // For files only
  CHAR8               Alias[160];    // For symlinks only
  UINT32              EquivId;       // For hardlinks: target object ID

  //
  // Version tracking for multi-version resolution during scan
  //
  UINT32              HeaderSeq;
  UINT32              HeaderPage;

  //
  // Tree structure
  //
  struct _YAFFS2_OBJECT *Parent;
  LIST_ENTRY          SiblingLink;   // Linked into parent's ChildHead
  LIST_ENTRY          ChildHead;     // Children list (directories only)

  //
  // Data chunks (files only)
  // Array indexed by [chunk_id - 1], ChunkCount = max chunk_id
  //
  UINT32              ChunkCount;
  UINT32              ChunkAlloc;    // Allocated capacity of Chunks array
  YAFFS2_CHUNK_LOC    *Chunks;
} YAFFS2_OBJECT;

//
// Open file instance (one per Open/OpenVolume call)
//
typedef struct {
  UINT32                Signature;
  EFI_FILE_PROTOCOL     Handle;
  UINT64                Position;
  YAFFS2_OBJECT         *OFile;
  YAFFS2_VOLUME         *Volume;
  LIST_ENTRY            *DirCursor;  // Current entry in OFile->ChildHead (dirs only)
} YAFFS2_IFILE;

#define YAFFS2_IFILE_FROM_HANDLE(a) \
  CR (a, YAFFS2_IFILE, Handle, YAFFS2_IFILE_SIGNATURE)

//
// Volume (one per NAND device with detected YAFFS2)
//
struct _YAFFS2_VOLUME {
  UINT32                             Signature;
  EFI_HANDLE                         Handle;
  MIKROTIK_NAND_FLASH_PROTOCOL       *Nand;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL    VolumeInterface;
  YAFFS2_OBJECT                      **Objects;     // Sparse array indexed by obj_id
  UINT32                             MaxObjId;      // Allocated size - 1
  YAFFS2_OBJECT                      *Root;         // Objects[1]
  UINT8                              *PageBuffer;   // PageSize bytes, reused for reads
};

#define YAFFS2_VOLUME_FROM_SFS(a) \
  CR (a, YAFFS2_VOLUME, VolumeInterface, YAFFS2_VOLUME_SIGNATURE)

// ---------------------------------------------------------------------------
// Scan.c
// ---------------------------------------------------------------------------

EFI_STATUS
Yaffs2DetectFilesystem (
  IN  MIKROTIK_NAND_FLASH_PROTOCOL  *Nand,
  IN  UINT8                         *PageBuffer
  );

EFI_STATUS
Yaffs2ScanNand (
  IN  YAFFS2_VOLUME  *Volume
  );

// ---------------------------------------------------------------------------
// Volume.c
// ---------------------------------------------------------------------------

EFI_STATUS
Yaffs2AllocateVolume (
  IN  EFI_HANDLE                     Handle,
  IN  MIKROTIK_NAND_FLASH_PROTOCOL   *Nand,
  OUT YAFFS2_VOLUME                  **VolumeOut
  );

VOID
Yaffs2FreeVolume (
  IN  YAFFS2_VOLUME  *Volume
  );

EFI_STATUS
EFIAPI
Yaffs2OpenVolume (
  IN  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *This,
  OUT EFI_FILE_PROTOCOL                **Root
  );

// ---------------------------------------------------------------------------
// Open.c
// ---------------------------------------------------------------------------

YAFFS2_IFILE *
Yaffs2AllocateIFile (
  IN  YAFFS2_VOLUME   *Volume,
  IN  YAFFS2_OBJECT   *OFile
  );

EFI_STATUS
EFIAPI
Yaffs2Open (
  IN  EFI_FILE_PROTOCOL  *This,
  OUT EFI_FILE_PROTOCOL  **NewHandle,
  IN  CHAR16             *FileName,
  IN  UINT64             OpenMode,
  IN  UINT64             Attributes
  );

EFI_STATUS
EFIAPI
Yaffs2Close (
  IN  EFI_FILE_PROTOCOL  *This
  );

EFI_STATUS
EFIAPI
Yaffs2Delete (
  IN  EFI_FILE_PROTOCOL  *This
  );

// ---------------------------------------------------------------------------
// ReadDir.c
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
Yaffs2Read (
  IN     EFI_FILE_PROTOCOL  *This,
  IN OUT UINTN              *BufferSize,
  OUT    VOID               *Buffer
  );

EFI_STATUS
EFIAPI
Yaffs2Write (
  IN     EFI_FILE_PROTOCOL  *This,
  IN OUT UINTN              *BufferSize,
  IN     VOID               *Buffer
  );

EFI_STATUS
EFIAPI
Yaffs2GetPosition (
  IN  EFI_FILE_PROTOCOL  *This,
  OUT UINT64             *Position
  );

EFI_STATUS
EFIAPI
Yaffs2SetPosition (
  IN  EFI_FILE_PROTOCOL  *This,
  IN  UINT64             Position
  );

EFI_STATUS
EFIAPI
Yaffs2Flush (
  IN  EFI_FILE_PROTOCOL  *This
  );

// ---------------------------------------------------------------------------
// Info.c
// ---------------------------------------------------------------------------

EFI_STATUS
EFIAPI
Yaffs2GetInfo (
  IN     EFI_FILE_PROTOCOL  *This,
  IN     EFI_GUID           *InformationType,
  IN OUT UINTN              *BufferSize,
     OUT VOID               *Buffer
  );

EFI_STATUS
EFIAPI
Yaffs2SetInfo (
  IN  EFI_FILE_PROTOCOL  *This,
  IN  EFI_GUID           *InformationType,
  IN  UINTN              BufferSize,
  IN  VOID               *Buffer
  );

#endif // MT_YAFFS2_DXE_H_
