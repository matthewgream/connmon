
#
# sudo apt install libmosquitto-dev libcurl4-openssl-dev libjson-c-dev libminiupnpc-dev
#

CC=gcc
CFLAGS_INCLUDE=
CFLAGS_OPT_COMMON=-Wall -Wextra -Wpedantic -O3 -fstack-protector-strong
CFLAGS_OPT_STRICT=\
    -Wstrict-prototypes -Wold-style-definition \
    -Wno-cast-align -Wcast-qual -Wconversion \
    -Wfloat-equal -Wformat=2 -Wformat-security \
    -Winit-self -Wjump-misses-init -Wlogical-op -Wmissing-include-dirs \
    -Wnested-externs -Wpointer-arith -Wredundant-decls -Wshadow \
    -Wstrict-overflow=2 -Wswitch-default \
    -Wswitch-enum -Wundef -Wunreachable-code -Wunused \
    -Wwrite-strings -Wno-stringop-truncation
CFLAGS=$(CFLAGS_INCLUDE) $(CFLAGS_OPT_COMMON) $(CFLAGS_OPT_STRICT)
LDFLAGS=-lmosquitto -lcurl -ljson-c -lminiupnpc
SOURCES=include/http_linux.h include/mqtt_linux.h include/util_linux.h include/config_linux.h
TARGET = connmon
HOSTNAME = $(shell hostname)

##

$(TARGET): $(TARGET).c $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(TARGET).c $(LDFLAGS)
all: $(TARGET)
clean:
	rm -f $(TARGET)
format:
	clang-format -i $(TARGET).c include/*.h
test: $(TARGET)
	./$(TARGET) --config $(TARGET).$(HOSTNAME).default
latency:
	journalctl -u $(TARGET) | analysis/latency.js
.PHONY: all clean format test lint latency

##

DEFAULT_DIR = /etc/default
TARGET_DIR = /usr/local/bin
SYSTEMD_DIR = /etc/systemd/system
define stop_systemd_service
	-systemctl stop $(1) 2>/dev/null || true
endef
define install_systemd_service
	-systemctl disable $(2) 2>/dev/null || true
	cp $(1).service $(SYSTEMD_DIR)/$(2).service
	systemctl daemon-reload
	systemctl enable $(2)
	systemctl start $(2) || echo "Warning: Failed to start $(2)"
endef
install_systemd_service: $(TARGET).service
	$(call install_systemd_service,$(TARGET),$(TARGET))
install_default: $(TARGET).$(HOSTNAME).default
	cp $(TARGET).$(HOSTNAME).default $(DEFAULT_DIR)/$(TARGET)
install_target: $(TARGET)
	$(call stop_systemd_service,$(TARGET))
	cp $(TARGET) $(TARGET_DIR)/$(TARGET)
install: install_target install_default install_systemd_service
restart:
	systemctl restart $(TARGET)
.PHONY: install install_target install_default install_systemd_service restart

