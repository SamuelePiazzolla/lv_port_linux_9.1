#
# Makefile definitivo per LVGL 9.1 su Linux
#

# =====================
# Toolchain
# =====================
CC ?= gcc

# =====================
# Directory principali
# =====================
BASE_DIR := $(shell pwd)
LVGL_DIR := $(BASE_DIR)/lvgl
UI_DIR := $(BASE_DIR)/ui
LOGIC_DIR := $(BASE_DIR)/logic
# Nuova directory per le librerie esterne
THIRD_PARTY_DIR := $(BASE_DIR)/third_party

# =====================
# Binario finale
# =====================
BIN := DEMO_UI

# =====================
# Flags di compilazione
# =====================
CFLAGS += -O2 -g 


# ---- CPU / ARCH (CRITICO per evitare SIGILL) ----
# Applica flag ARM solo se il compilatore è aarch64
ifneq (,$(findstring aarch64,$(CC)))
CFLAGS += -march=armv8-a
CFLAGS += -mcpu=cortex-a55
CFLAGS += -mno-outline-atomics
endif
# -----------------------------------------------

CFLAGS += -I$(BASE_DIR) 
CFLAGS += -I$(LVGL_DIR) 
CFLAGS += -I$(UI_DIR)
CFLAGS += -I$(LOGIC_DIR)

# Percorso header libyuv (Fondamentale per velocizzare conversione YUYV -> XRGB)
CFLAGS += -I$(THIRD_PARTY_DIR)/libyuv/include

# GLib / GIO headers
CFLAGS += -I$(THIRD_PARTY_DIR)/sysroot/usr/include/glib-2.0
CFLAGS += -I$(THIRD_PARTY_DIR)/sysroot/usr/lib64/glib-2.0/include

# GPIOD headers
CFLAGS += -I$(THIRD_PARTY_DIR)/sysroot/usr/include

CFLAGS += -Wall -Wextra -Wshadow -Wundef -Wmissing-prototypes -Wno-unused-function -Wpointer-arith -fno-strict-aliasing 

CFLAGS += -D_GNU_SOURCE

# =====================
# Flags di linking
# =====================
# Percorso libreria e link ai binari .a (Fondamentale per il linker)
# -lstdc++ serve perché libyuv internamente usa C++
LDFLAGS += -L$(THIRD_PARTY_DIR)/libyuv/lib -lyuv -lstdc++

# GLib / GIO (GDBus)
LDFLAGS += -L$(THIRD_PARTY_DIR)/sysroot/usr/lib64 -lgio-2.0 -lgobject-2.0 -lglib-2.0

# GPIO
LDFLAGS += -L$(THIRD_PARTY_DIR)/sysroot/usr/lib64 -lgpiod

LDFLAGS += -lasound -lm -pthread 

# =====================
# Funzione ricorsiva per .c NON-LVGL
# =====================
define recurse
$(wildcard $(1)/*.c) \
$(foreach d,$(wildcard $(1)/*),$(call recurse,$(d)))
endef

# =====================
# LVGL (makefile ufficiale)
# =====================
LVGL_DIR_NAME := lvgl
include $(LVGL_DIR)/lvgl.mk

# =====================
# Codice applicativo
# =====================
CSRCS += $(call recurse,$(UI_DIR))
CSRCS += $(call recurse,$(LOGIC_DIR))
CSRCS += $(BASE_DIR)/main.c

# =====================
# Oggetti
# =====================
OBJEXT := .o
OBJS := $(CSRCS:.c=$(OBJEXT))

# =====================
# Regole
# =====================
all: $(BIN)

%.o: %.c
	@echo "Compiling $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJS)
	@echo "Linking $(BIN)"
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

clean:
	rm -f $(BIN) $(OBJS)