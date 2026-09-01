UK_ROOT ?= $(CURDIR)/.unikraft/unikraft
UK_LIBS ?= $(CURDIR)/.libs
ENA_DIR ?= $(realpath $(CURDIR)/../..)
LIBS := $(UK_LIBS)/lib-lwip:$(ENA_DIR)

.PHONY: all pre-patches

all: pre-patches
	@$(MAKE) -C $(UK_ROOT) A=$(CURDIR) L=$(LIBS)

pre-patches:
	@./scripts/apply_patches.sh

ifneq (,$(filter-out pre-patches,$(MAKECMDGOALS)))
$(filter-out pre-patches,$(MAKECMDGOALS)): pre-patches
	@$(MAKE) -C $(UK_ROOT) A=$(CURDIR) L=$(LIBS) $(filter-out pre-patches,$(MAKECMDGOALS))
endif
