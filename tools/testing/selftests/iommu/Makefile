# SPDX-License-Identifier: GPL-2.0-only
CFLAGS += -Wall -O2 -Wno-unused-function
CFLAGS += $(KHDR_INCLUDES)
LDLIBS += -lcap

TEST_GEN_PROGS :=
TEST_GEN_PROGS += iommufd
TEST_GEN_PROGS += iommufd_fail_nth

TEST_GEN_PROGS_EXTENDED += iommufd_liveupdate_kexec_test

include ../lib.mk
include ../liveupdate/lib/libliveupdate.mk

CFLAGS += -I$(top_srcdir)/tools/include
CFLAGS += -MD
CFLAGS += $(EXTRA_CFLAGS)

TEST_O := $(patsubst %, %.o, $(TEST_GEN_PROGS_EXTENDED))

TEST_DEP_FILES := $(patsubst %.o, %.d, $(LIBLIVEUPDATE_O))
TEST_DEP_FILES += $(patsubst %.o, %.d, $(TEST_O))
-include $(TEST_DEP_FILES)

$(TEST_GEN_PROGS_EXTENDED): $(OUTPUT)/%: %.o $(LIBLIVEUPDATE_O)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) $(TARGET_ARCH) $< $(LIBLIVEUPDATE_O) $(LDLIBS) -o $@

EXTRA_CLEAN += $(TEST_O)
EXTRA_CLEAN += $(TEST_DEP_FILES)
