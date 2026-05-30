CC ?= cc
CFLAGS ?= -Wall -Wextra -std=c11
LDFLAGS ?=
LDLIBS ?= -lX11 -lXrandr -lm

APP := flashguard
SRC := flashguard.c
BIN := $(APP)

PREFIX ?= $(HOME)/.local
DESTDIR ?=
SYSTEMD_USER_DIR ?= $(HOME)/.config/systemd/user
XDG_CONFIG_HOME ?= $(HOME)/.config
CONFIG_DIR := $(XDG_CONFIG_HOME)/flashguard
CONFIG_FILE := $(CONFIG_DIR)/config

.PHONY: all build clean run install install-bin install-config install-service uninstall-service enable-service disable-service restart-service logs status

all: build

build: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

run: build
	./$(BIN)

install: install-bin install-config install-service

install-bin: build
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(APP)

install-config:
	install -d $(CONFIG_DIR)
	@if [ ! -f "$(CONFIG_FILE)" ]; then \
		printf '%s\n' \
			'# FlashGuard config' \
			'fps=144' \
			'min_brightness=0.6' \
			'max_brightness=1.0' \
			'curve_start_avg=0.10' \
			'curve_end_avg=0.95' \
			'curve_gamma=0.5' \
			'curve_shoulder=0.35' \
			'avg_input_gamma=1.0' \
			'avg_bias=0.0' \
			'brightness_smoothing=0.0' \
			'grid_x=40' \
			'grid_y=24' \
			> "$(CONFIG_FILE)"; \
		echo "Created $(CONFIG_FILE)"; \
	else \
		echo "Keeping existing $(CONFIG_FILE)"; \
	fi

install-service:
	install -d $(SYSTEMD_USER_DIR)
	install -m 0644 flashguard.service $(SYSTEMD_USER_DIR)/flashguard.service
	systemctl --user daemon-reload

uninstall-service:
	-systemctl --user disable --now flashguard.service
	-rm -f $(SYSTEMD_USER_DIR)/flashguard.service
	-systemctl --user daemon-reload

enable-service:
	systemctl --user enable --now flashguard.service

disable-service:
	systemctl --user disable --now flashguard.service

restart-service:
	systemctl --user restart flashguard.service

status:
	systemctl --user status flashguard.service --no-pager

logs:
	journalctl --user -u flashguard.service -f

clean:
	rm -f $(BIN)