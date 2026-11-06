[Defines]
  PLATFORM_NAME                  = MT6765Pkg
  PLATFORM_GUID                  = 28f1a3bf-193a-47e3-a7b9-5a435eaab2ee
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = MT6765Pkg/MT6765Pkg.fdf

!include MT6765Pkg/MT6765Pkg.dsc

#
# Samsung Galaxy A03s (SM-A037F), MT6765, 720x1600, 4 GB DRAM.
#
# Display values come from the *runtime* device tree, not from the EVB
# defaults that are also present in the stock DTB:
#   atag,videolfb = fb_base 0x7C250000, vram 0x00FB0000
#   lcm = nl9911c_hdplus1600_dsi_vdo_truly_truly
# FrameBufferSerialPortLib writes 32-bit pixels with a width*4 stride, so the
# PCD must be the exact fb_base LK hands to the kernel.  The old reference
# value 0x7C2D4800 is roo's fb_base, not this device's.
#
[PcdsFixedAtBuild.common]
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0xC0000000
  gEmbeddedTokenSpaceGuid.PcdPrePiStackBase|0x40C00000
  gEmbeddedTokenSpaceGuid.PcdPrePiStackSize|0x00040000
  gMT6765PkgTokenSpaceGuid.PcdUefiMemPoolBase|0x40D00000
  gMT6765PkgTokenSpaceGuid.PcdUefiMemPoolSize|0x03300000
  gArmTokenSpaceGuid.PcdCpuVectorBaseAddress|0x40C40000

  #
  # DebugLib writes through SerialPortLib, which on this board is the
  # framebuffer console library.  The common DSC enables DEBUG_INFO, so
  # every CoreInstallProtocolInterface() call flushes a full screen of
  # pixels and the boot takes minutes instead of seconds.  Keep only
  # DEBUG_ERROR: real failures still reach the panel, routine chatter
  # does not.
  #
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x80000000

  gMT6765PkgTokenSpaceGuid.PcdMipiFrameBufferAddress|0x7C250000
  gMT6765PkgTokenSpaceGuid.PcdMipiFrameBufferWidth|720
  gMT6765PkgTokenSpaceGuid.PcdMipiFrameBufferHeight|1600
  gMT6765PkgTokenSpaceGuid.PcdMipiFrameBufferVisibleWidth|720
  gMT6765PkgTokenSpaceGuid.PcdMipiFrameBufferVisibleHeight|1600
  gMT6765PkgTokenSpaceGuid.PcdMipiFrameBufferPixelBpp|32
  # fb0 sysfs reports stride 2944 bytes and virtual_size 736x4800, so the
  # panel line pitch is 736 pixels even though only 720 are visible.
  gMT6765PkgTokenSpaceGuid.PcdMipiFrameBufferStride|736

  #
  # Interrupt controller.  The A03s DTB says arm,gic-v3 with the
  # distributor at 0x0C000000 and one redistributor region at
  # 0x0C100000 (size 0x200000).  The common DSC only sets the GICv2 CPU
  # interface base; without the redistributor base, ArmGicV3Dxe would
  # probe address 0 and no interrupt could ever be registered.
  #
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x0C000000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x0C100000

  #
  # Generic timer PPIs.  The DTB lists <1 13>, <1 14>, <1 11>, <1 10>,
  # i.e. secure/hyp/virt/non-secure = 13/10/11/14; AArch64 INTIDs are
  # 16 + PPI, so 29/26/27/30.  The common DSC's 0x12/0x13 (18/19) are
  # leftovers from another SoC.  The counter runs at 13 MHz.
  #
  gArmTokenSpaceGuid.PcdArmArchTimerSecIntrNum|29
  gArmTokenSpaceGuid.PcdArmArchTimerHypIntrNum|26
  gArmTokenSpaceGuid.PcdArmArchTimerVirtIntrNum|27
  gArmTokenSpaceGuid.PcdArmArchTimerIntrNum|30
  gArmTokenSpaceGuid.PcdArmArchTimerFreqInHz|13000000

[BuildOptions]
  # Selects the A03s descriptor table in Include/Configuration/DeviceMemoryMap.h.
  GCC:*_*_AARCH64_CC_FLAGS = -DMT6765_DEVICE_A03S
  #
  # GCC LTO materialises calls to memcpy/memset only during final code
  # generation, after --start-group has already been scanned.  The upstream
  # liblto-aarch64.a pass-through trick does not pull the definitions back in
  # with this toolchain, so keep the intrinsics rooted explicitly.
  #
  GCC:*_*_AARCH64_DLINK_FLAGS = -Wl,-u,memcpy -Wl,-u,memset
