UK_ROOT ?= $(CURDIR)/.unikraft/unikraft
UK_LIBS ?= $(CURDIR)/.libs
ENA_DIR ?= $(realpath $(CURDIR)/../..)
LIBS := $(UK_LIBS)/lib-lwip:$(ENA_DIR)

.PHONY: all pre-patches clean

all: pre-patches
	@$(MAKE) -C $(UK_ROOT) A=$(CURDIR) L=$(LIBS)

pre-patches:
	@./scripts/apply_patches.sh

clean:
	@if [ -d "$(UK_ROOT)" ]; then $(MAKE) -C $(UK_ROOT) A=$(CURDIR) L=$(LIBS) clean; fi
	@rm -rf build

ifneq (,$(filter-out pre-patches clean,$(MAKECMDGOALS)))
$(filter-out pre-patches clean,$(MAKECMDGOALS)): pre-patches
	@$(MAKE) -C $(UK_ROOT) A=$(CURDIR) L=$(LIBS) $(filter-out pre-patches clean,$(MAKECMDGOALS))
endif
