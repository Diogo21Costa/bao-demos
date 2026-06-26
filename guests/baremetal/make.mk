baremetal_src:=$(bao_demos)/guests/baremetal/local_srcs

baremetal_bin:=$(baremetal_src)/build/$(PLATFORM)/baremetal.bin

define build-baremetal
$(strip $1):
	$(MAKE) -C $(baremetal_src) PLATFORM=$(PLATFORM) $(strip $2)
	cp $(baremetal_bin) $$@
endef
