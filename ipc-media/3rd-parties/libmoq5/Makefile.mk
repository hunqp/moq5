CXXFLAGS += -Isources/3rd-party/libmoq5/include

LDFLAGS += -Lsources/3rd-party/libmoq5/lib -Wl,--gc-sections
LDFLAGS += -Lsources/3rd-party/libopenssl-1.1.1h/lib
LDLIBS += -lmoq-service -lmoq-adapter-picoquic-threaded -lmoq-msf -lmoq-media-object -lmoq-cmaf-packager -lmoq-cmaf -lmoq-codec-signaling -lmoq-loc
LDLIBS += -lmoq-adapter-picoquic -lmoq-core
LDLIBS += -lpicoquic-core -lpicotls-openssl -lpicotls-core -lpicotls-minicrypto
LDLIBS += -lssl -lcrypto -lpthread
