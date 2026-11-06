#ifndef _DEVICE_MEMORY_MAP_H_
#define _DEVICE_MEMORY_MAP_H_

#include <Library/ArmLib.h>

#include <Configuration/XnuPayload.h>

#define MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT 128

/* Below flag is used for system memory */
#define SYSTEM_MEMORY_RESOURCE_ATTR_CAPABILITIES                               \
  EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_INITIALIZED |        \
      EFI_RESOURCE_ATTRIBUTE_TESTED | EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE |     \
      EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE |                               \
      EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE |                         \
      EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE |                            \
      EFI_RESOURCE_ATTRIBUTE_EXECUTION_PROTECTABLE

typedef enum { NoHob, AddMem, AddDev, HobOnlyNoCacheSetting, MaxMem } DeviceMemoryAddHob;

#define MEMORY_REGION_NAME_MAX_LENGTH 32

typedef struct {
  CHAR8                        Name[MEMORY_REGION_NAME_MAX_LENGTH];
  EFI_PHYSICAL_ADDRESS         Address;
  UINT64                       Length;
  DeviceMemoryAddHob           HobOption;
  EFI_RESOURCE_TYPE            ResourceType;
  EFI_RESOURCE_ATTRIBUTE_TYPE  ResourceAttribute;
  EFI_MEMORY_TYPE              MemoryType;
  ARM_MEMORY_REGION_ATTRIBUTES ArmAttributes;
} ARM_MEMORY_REGION_DESCRIPTOR_EX, *PARM_MEMORY_REGION_DESCRIPTOR_EX;

#define MEM_RES EFI_RESOURCE_MEMORY_RESERVED
#define MMAP_IO EFI_RESOURCE_MEMORY_MAPPED_IO
#define SYS_MEM EFI_RESOURCE_SYSTEM_MEMORY

#define SYS_MEM_CAP SYSTEM_MEMORY_RESOURCE_ATTR_CAPABILITIES
#define INITIALIZED EFI_RESOURCE_ATTRIBUTE_INITIALIZED
#define WRITE_COMBINEABLE EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE
#define UNCACHEABLE EFI_RESOURCE_ATTRIBUTE_UNCACHEABLE

#define Reserv EfiReservedMemoryType
#define Conv EfiConventionalMemory
#define BsData EfiBootServicesData
#define BsCode EfiBootServicesCode
#define RtData EfiRuntimeServicesData
#define RtCode EfiRuntimeServicesCode
#define MmIO EfiMemoryMappedIO

#define NS_DEVICE ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_DEVICE
#define DEVICE ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_DEVICE
#define WRITE_THROUGH ARM_MEMORY_REGION_ATTRIBUTE_WRITE_THROUGH
#define WRITE_THROUGH_XN ARM_MEMORY_REGION_ATTRIBUTE_WRITE_THROUGH
#define WRITE_BACK ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK
#define WRITE_BACK_XN ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK
#define UNCACHED_UNBUFFERED ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED
#define UNCACHED_UNBUFFERED_XN ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED

#if defined(MT6765_DEVICE_A03S)
/*
 * Samsung Galaxy A03s (SM-A037F), MT6765, 4 GB DRAM.
 *
 * The low 256 MB window is the same as the reference MT6765 port and is
 * where the BootShim, UEFI heap and UEFI FD live.  The framebuffer is the
 * value LK publishes in the runtime atag,videolfb property:
 *   fb_base = 0x7C250000, vram = 0x00FB0000, lcm = nl9911c_hdplus1600...
 * Only the regions UEFI actually touches are mapped; mapping the whole
 * 4 GB would claim TEE/modem/ramdisk memory as conventional RAM.
 */
static ARM_MEMORY_REGION_DESCRIPTOR_EX gDeviceMemoryDescriptorEx[] = {
/*                                                    EFI_RESOURCE_ EFI_RESOURCE_ATTRIBUTE_ EFI_MEMORY_TYPE ARM_REGION_ATTRIBUTE_
     MemLabel(32 Char.),  MemBase,    MemSize, BuildHob, ResourceType, ResourceAttribute, MemoryType, CacheAttributes
*/

//--------------------- Register ---------------------
    {"Periphs",           0x00000000, 0x1B000000,  AddMem, MEM_RES, UNCACHEABLE,  RtCode,   NS_DEVICE},

//--------------------- DDR --------------------- */
    {"HLOS 0",            0x40000000, 0x00C00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
    {"UEFI Stack",        0x40C00000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
    {"CPU Vectors",       0x40C40000, 0x00010000, AddMem, SYS_MEM, SYS_MEM_CAP, BsCode, WRITE_BACK},
    {"HLOS 1",            0x40C50000, 0x0F3B0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"UEFI FD",           0x50000000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, BsCode, WRITE_BACK},

    /*
     * The rest of the stock device tree's low memory node, which ends at
     * 0x60000000.  This is ordinary RAM (no fixed reserved-memory node
     * covers it) but was left unmapped, so the UEFI allocator could not use
     * it and the only free block in HLOS 1 was the 96 MB gap between the
     * UEFI pool and its page tables.  XNU needs one contiguous window of
     * about 127 MB; this region gives it 224 MB (32 MB aligned) below the
     * XNUP payload at 0x60000000.
     */
    {"HLOS 2",            0x50200000, 0x0FE00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},

    /* LK framebuffer: 0x7C250000 + 0x00FB0000 (from atag,videolfb). */
    {"Display Reserved",  0x7C250000, 0x00FB0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},

    /*
     * The upper DDR the stock device tree does not describe (its /memory node
     * says 512 MB, the PCD says 3 GB).  This range is above the TEE and SCP
     * reservations of the reference MT6765 layout, and the port runs no MTK
     * firmware of its own: it is ordinary RAM for the kernel.
     */
    /* /proc/iomem: 0xd0000000-0xfeff5fff is contiguous System RAM with no carveouts (736 MB aligned). */
    {"HLOS 3",            0xD0000000, 0x2E000000, NoHob,  SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},

    /*
     * Area the HackiOS loader looks at for its "XNUP" payload.  It sits in
     * the low DRAM bank, between the UEFI image and the display, and is
     * deliberately not SP-MEM: marking it reserved keeps the UEFI allocator
     * out of it, while still mapping it so the loader can probe the magic
     * instead of taking a translation fault.
     */
    {"XNU Payload",       XNU_PAYLOAD_BASE, XNU_PAYLOAD_MAX_SIZE, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},

    /* Terminator for MMU */
    { "Terminator", 0, 0, 0, 0, 0, 0, 0}};
#else
static ARM_MEMORY_REGION_DESCRIPTOR_EX gDeviceMemoryDescriptorEx[] = {
/*                                                    EFI_RESOURCE_ EFI_RESOURCE_ATTRIBUTE_ EFI_MEMORY_TYPE ARM_REGION_ATTRIBUTE_
     MemLabel(32 Char.),  MemBase,    MemSize, BuildHob, ResourceType, ResourceAttribute, MemoryType, CacheAttributes
*/

//--------------------- Register ---------------------
    {"Periphs",           0x00000000, 0x1B000000,  AddMem, MEM_RES, UNCACHEABLE,  RtCode,   NS_DEVICE},

//--------------------- DDR --------------------- */

// DDR HAVE: 0x40000000 0xc0000000
    {"HLOS 0",            0x40000000, 0x00C00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
    {"UEFI Stack",        0x40C00000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
    {"CPU Vectors",       0x40C40000, 0x00010000, AddMem, SYS_MEM, SYS_MEM_CAP, BsCode, WRITE_BACK},
    {"HLOS 1",            0x40C50000, 0x0F3B0000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
    {"UEFI FD",           0x50000000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, BsCode, WRITE_BACK},


    {"ATF Reserved",      0x47D80000, 0x00050000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
    {"SPM Reserved",      0x77FF0000, 0x00010000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
    {"TEE Reserved",      0x7CD00000, 0x03200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
    {"SSPM Reserved",     0x7FF00000, 0x000C0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
    {"SCP Reserved",      0x9F900000, 0x00600000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},

    {"Display Reserved",  0x7BEE0000, 0x00E20000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},

    /* Terminator for MMU */
    { "Terminator", 0, 0, 0, 0, 0, 0, 0}};
#endif

#endif
