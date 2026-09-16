# Native Linux build. Keeps the sibling libaochat checkout read-only.
CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=
LIBAOCHAT_DIR ?= ../libaochat

BUILD := .build
LIBBUILD := $(BUILD)/libaochat
PLUGIN_OUT := kworker-plugins
PLUGIN_BUILD := $(BUILD)/plugins
LIBNAMES := connect event keyex list misc msg_queue packet send_queue timer
LIBSRC := $(addprefix $(LIBBUILD)/,$(addsuffix .c,$(LIBNAMES)))
LIBOBJ := $(LIBSRC:.c=.o)
LIBRARY := $(LIBBUILD)/libaochat.a
HOSTOBJ := $(BUILD)/main.o $(BUILD)/config.o $(BUILD)/plugin_loader.o
PLUGINS := $(PLUGIN_OUT)/buddy.so $(PLUGIN_OUT)/tell.so $(PLUGIN_OUT)/nady_proxy.so

.PHONY: all clean check
.DELETE_ON_ERROR:
.SECONDARY: $(LIBSRC)

all: kworker $(PLUGINS)

kworker: $(HOSTOBJ) $(LIBRARY)
	$(CC) $(CFLAGS) $(LDFLAGS) -o "$@" $(HOSTOBJ) $(LIBRARY) \
	  $(LDLIBS) -ldl -lpthread -lcrypto

$(BUILD)/%.o: src/%.c src/kworker_internal.h include/kworker_plugin.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Wall -Wextra -Wpedantic \
	  -Iinclude -I"$(LIBBUILD)" -MMD -MP -c "$<" -o "$@"

# Ensure the dependency header is staged before compiling the only host
# source that includes it; never silently fall through to /usr/include/aochat.h.
$(BUILD)/main.o: $(LIBBUILD)/aochat.h

$(PLUGIN_OUT)/buddy.so: plugins/buddy.c include/kworker_plugin.h include/kworker_services.h | $(PLUGIN_OUT) $(PLUGIN_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Wall -Wextra -Wpedantic -fPIC \
	  -Iinclude -shared "$<" -o "$(PLUGIN_BUILD)/buddy.so" -lpthread
	mv -f "$(PLUGIN_BUILD)/buddy.so" "$@"

$(PLUGIN_OUT)/tell.so: plugins/tell.c include/kworker_plugin.h include/kworker_services.h | $(PLUGIN_OUT) $(PLUGIN_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Wall -Wextra -Wpedantic -fPIC \
	  -Iinclude -shared "$<" -o "$(PLUGIN_BUILD)/tell.so" -lpthread
	mv -f "$(PLUGIN_BUILD)/tell.so" "$@"

$(PLUGIN_OUT)/nady_proxy.so: plugins/nady_proxy.c include/kworker_plugin.h include/kworker_services.h | $(PLUGIN_OUT) $(PLUGIN_BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Wall -Wextra -Wpedantic -fPIC \
	  -Iinclude -shared "$<" -o "$(PLUGIN_BUILD)/nady_proxy.so" -lpthread
	mv -f "$(PLUGIN_BUILD)/nady_proxy.so" "$@"

$(LIBBUILD)/%.c: $(LIBAOCHAT_DIR)/%.c | $(LIBBUILD)
	cp "$<" "$@"

$(LIBBUILD)/aochat.h: $(LIBAOCHAT_DIR)/aochat.h | $(LIBBUILD)
	cp "$<" "$@"

$(LIBBUILD)/%.o: $(LIBBUILD)/%.c $(LIBBUILD)/aochat.h $(LIBBUILD)/autoconf.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu99 -D_DEFAULT_SOURCE -D__COMPILING_LIB \
	  -fno-strict-aliasing -I"$(LIBBUILD)" -MMD -MP -c "$<" -o "$@"

$(LIBRARY): $(LIBOBJ)
	$(AR) rcs "$@" $^

$(LIBBUILD)/autoconf.h: | $(LIBBUILD)
	printf '%s\n' \
	  '#ifndef KWORKER_LIBAOCHAT_AUTOCONF_H' \
	  '#define KWORKER_LIBAOCHAT_AUTOCONF_H' \
	  '#include <stdint.h>' \
	  '#define HAVE_SYS_TYPES_H 1' \
	  '#define HAVE_UNISTD_H 1' \
	  '#define HAVE_STDINT_H 1' \
	  '#define HAVE_INET_ATON 1' \
	  '#define HAVE_SNPRINTF 1' \
	  '#define HAVE_VSNPRINTF 1' \
	  '#define HAVE_LIBCRYPTO 1' \
	  '#define HAVE_OPENSSL_BN_H 1' \
	  '#endif' > "$@"

$(BUILD) $(LIBBUILD) $(PLUGIN_BUILD) $(PLUGIN_OUT):
	mkdir -p "$@"

check: | $(BUILD)
	python3 tests/test_kworker_policy.py
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Wall -Wextra -Wpedantic -Werror \
	  -Iinclude tests/test_nady_proxy_routes.c -o "$(BUILD)/test_nady_proxy_routes" -lpthread
	"$(BUILD)/test_nady_proxy_routes"
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Wall -Wextra -Wpedantic -Werror \
	  -Iinclude tests/test_tell_parallel.c -o "$(BUILD)/test_tell_parallel" -lpthread
	"$(BUILD)/test_tell_parallel"
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Wall -Wextra -Wpedantic -Werror \
	  -Iinclude tests/test_config.c src/config.c -o "$(BUILD)/test_config"
	"$(BUILD)/test_config"
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Wall -Wextra -Wpedantic -Werror \
	  -Iinclude tests/test_buddy_scale.c -o "$(BUILD)/test_buddy_scale" -lpthread
	"$(BUILD)/test_buddy_scale"

clean:
	rm -f -- kworker
	rm -rf -- $(BUILD) $(PLUGIN_OUT)

-include $(HOSTOBJ:.o=.d) $(LIBOBJ:.o=.d)
