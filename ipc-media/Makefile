CROSS_COMPILER ?= arm-rockchip830-linux-uclibcgnueabihf
CC    := $(CROSS_COMPILER)-gcc
CXX   := $(CROSS_COMPILER)-g++
STRIP := $(CROSS_COMPILER)-strip

# Directory holding debugger.h (included by AacDts/aac_encoder.cpp)
DEBUGGER_INC ?= .

BUILD_DIR := build
TARGET    := $(BUILD_DIR)/ipc-media
LIBS_DIR  := 3rd-parties

# Size-optimized: per-function sections plus --gc-sections drop unused library code
CFLAGS   := -Os -ffunction-sections -fdata-sections
CXXFLAGS := -Os -ffunction-sections -fdata-sections -std=c++11 -I$(DEBUGGER_INC) -IAacDts -IMoqPublisher \
            -I$(LIBS_DIR)/libkiwi -I$(LIBS_DIR)/libfaac/include -I$(LIBS_DIR)/libmoq5/include

LDFLAGS := -Wl,--gc-sections -L$(LIBS_DIR)/libfaac/lib -L$(LIBS_DIR)/libmoq5/lib -L$(LIBS_DIR)/libopenssl-1.1.1h/lib
LDLIBS  := -lfaac \
           -lmoq-service -lmoq-adapter-picoquic-threaded -lmoq-msf -lmoq-media-object -lmoq-cmaf-packager -lmoq-cmaf -lmoq-codec-signaling -lmoq-loc \
           -lmoq-adapter-picoquic -lmoq-core \
           -lpicoquic-core -lpicotls-openssl -lpicotls-core -lpicotls-minicrypto \
           -lssl -lcrypto -lpthread

CXX_SRCS := main.cpp AacDts/aac_encoder.cpp AacDts/aac_adts.cpp MoqPublisher/codec.cpp MoqPublisher/moq_publisher.cpp
C_SRCS   := $(LIBS_DIR)/libkiwi/kiwi_ringbuffer.c
OBJS     := $(addprefix $(BUILD_DIR)/,$(CXX_SRCS:.cpp=.o) $(C_SRCS:.c=.o))

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)
	$(STRIP) $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
