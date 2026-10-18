#   make            snd-audiolaz.ko
#   make clean
obj-m := snd-audiolaz.o
KDIR ?= /lib/modules/$(shell uname -r)/build
LLVM_OPT := $(shell grep -qs '^CONFIG_CC_IS_CLANG=y' $(KDIR)/.config && echo LLVM=1)

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(LLVM_OPT) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) $(LLVM_OPT) clean
