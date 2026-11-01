HEAP_SIZE      = 8388208
STACK_SIZE     = 61800

PRODUCT = BssGap.pdx

SDK = ${PLAYDATE_SDK_PATH}
ifeq ($(SDK),)
	SDK = $(shell egrep '^\s*SDKRoot' ~/.Playdate/config | head -n 1 | cut -c9-)
endif
ifeq ($(SDK),)
$(error SDK path not found; set ENV value PLAYDATE_SDK_PATH)
endif

VPATH += src
SRC = src/main.c

include $(SDK)/C_API/buildsupport/common.mk

# Compare the memory size pdc wrote into the pdex.bin header (offset 0x24)
# with where .bss really ends in the ELF.
layout: device
	@hdr=$$(xxd -s 0x20 -l 8 -e $(PRODUCT)/pdex.bin | awk '{ print $$2, $$3 }'); \
	set -- $$hdr; \
	data_end=$$(arm-none-eabi-nm $(OBJDIR)/pdex.elf | awk '$$3 == "__data_end__" { print $$1 }'); \
	bss=$$(arm-none-eabi-objdump -h $(OBJDIR)/pdex.elf | awk '$$2 == ".bss" { print $$3, $$4 }'); \
	set -- $$1 $$2 $$bss; \
	printf '.data ends        0x%s\n.bss starts       0x%s (size 0x%s)\n.bss ends         0x%08x\npdex.bin header:  file size 0x%s, memory size 0x%s\n=> %d bytes of .bss outside the reserved memory\n' \
		$$data_end $$4 $$3 $$((0x$$4 + 0x$$3)) $$1 $$2 $$((0x$$4 + 0x$$3 - 0x$$2))

.PHONY: layout
