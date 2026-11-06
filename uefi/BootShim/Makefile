ARCH              = arm64
TARGET            = aarch64-linux-gnu
CROSS_COMPILE     = $(TARGET)-
CC                = $(CROSS_COMPILE)gcc
OBJCOPY           = $(CROSS_COMPILE)objcopy

# The shim copies UEFI_SIZE bytes of firmware to UEFI_BASE, then PAYLOAD_SIZE
# bytes of XNUP payload (appended after the firmware volume) to
# XNU_PAYLOAD_BASE.  build_a03s.sh passes the real payload size; the defaults
# keep a plain `make` working.
UEFI_BASE         ?= 0x50000000
UEFI_SIZE         ?= 0x00200000
XNU_PAYLOAD_BASE  ?= 0x60000000
PAYLOAD_SIZE      ?= 0

all: BootShim.elf BootShim.bin

BootShim.bin: BootShim.elf
	"$(OBJCOPY)" -O binary "$<" "$@"

BootShim.elf: BootShim.S
	"$(CC)" -c "$<" -o "$@" -DUEFI_BASE=$(UEFI_BASE) -DUEFI_SIZE=$(UEFI_SIZE) \
	  -DXNU_PAYLOAD_BASE=$(XNU_PAYLOAD_BASE) -DPAYLOAD_SIZE=$(PAYLOAD_SIZE)

BootShim.S:
