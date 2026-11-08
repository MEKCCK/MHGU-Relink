# SPDX-License-Identifier: GPL-2.0-or-later
# Linux/POSIX build of the XX/GU gateway core (CLI only; no Win32 GUI, no tests).
# The upstream build.ps1 is Windows-only; this Makefile compiles the same
# production units, plus the vendored ENet, against the system zstd.

CXX      ?= g++
CC       ?= gcc
ENET     := room-transport/vendor/enet-2662c0de09e36f2a2030ccc2c528a3e4c9e8138a
FMT      := room-transport/vendor/fmt-e69e5f977d458f2650bb346dadf2ad30c5320281
OUT      := build/obj
BIN      := build/citra-gateway
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -DNOMINMAX -DFMT_HEADER_ONLY -fPIC -MMD -MP
CFLAGS   ?= -std=c99 -O2 -g -fPIC -D_DEFAULT_SOURCE \
            -DHAS_GETADDRINFO -DHAS_GETNAMEINFO -DHAS_GETHOSTBYADDR_R -DHAS_GETHOSTBYNAME_R \
            -DHAS_POLL -DHAS_FCNTL -DHAS_INET_PTON -DHAS_INET_NTOP -DHAS_MSGHDR_FLAGS -DHAS_SOCKLEN_T
INCLUDES := -Icitra-gateway/include -Icitra-room/include -Iroom-client/include \
            -Iroom-transport/include -Icitra-control/include -Iroom-transport/support/include \
            -I$(ENET)/include -I$(FMT)/include
LDLIBS   := -lzstd -lpthread

CXX_SRCS := \
  citra-room/src/protocol.cpp citra-room/src/session.cpp \
  room-client/src/protocol.cpp room-client/src/session.cpp \
  room-transport/src/transport.cpp \
  citra-control/src/bytes.cpp citra-control/src/crypto.cpp citra-control/src/ldn_ctrl.cpp \
  citra-control/src/map.cpp citra-control/src/state.cpp citra-control/src/heartbeat.cpp \
  citra-control/src/uds_frames.cpp \
  citra-gateway/src/trace.cpp citra-gateway/src/identity.cpp citra-gateway/src/control.cpp \
  citra-gateway/src/gateway.cpp citra-gateway/src/main.cpp

ENET_SRCS := callbacks compress host list packet peer protocol unix

CXX_OBJS := $(patsubst %.cpp,$(OUT)/%.o,$(CXX_SRCS))
ENET_OBJS := $(patsubst %,$(OUT)/enet/%.o,$(ENET_SRCS))
OBJS := $(CXX_OBJS) $(ENET_OBJS)

all: $(BIN)

$(BIN): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OUT)/enet/%.o: $(ENET)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(ENET)/include -c $< -o $@

clean:
	rm -rf build

-include $(OBJS:.o=.d)

.PHONY: all clean
