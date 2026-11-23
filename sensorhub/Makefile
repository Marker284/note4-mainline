# Convenience wrapper; the real build description is in Kbuild.
KDIR ?= $(CURDIR)/../build/kernel
ARCH ?= arm
LLVM ?= 1

# The kernel tree has no Module.symvers (only vmlinux was built), so
# references to in-kernel/IIO exports cannot be resolved by modpost.
KBUILD_MODPOST_WARN ?= 1
export KBUILD_MODPOST_WARN

all: modules

modules clean:
	$(MAKE) -C $(KDIR) ARCH=$(ARCH) LLVM=$(LLVM) LOCALVERSION= M=$(CURDIR) $@

.PHONY: all modules clean
