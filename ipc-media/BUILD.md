# Building ipc-media

ipc-media is the RV1106 camera app that publishes the encoder output to a MoQ relay:
- H.264 or H.265 video from `/tmp/venc*.shm`.
- G.711 or PCM audio from `/tmp/aenc0.shm`, transcoded to AAC.

It uses MoQT draft 22 with CMAF packaging. It is built in two steps:
1. Cross-build libmoq (this repository) into `3rd-parties/libmoq5/`.
2. Build the app with `make`.

## Prerequisites

You need the following on the build host:

| Item | Notes |
|---|---|
| Toolchain `arm-rockchip830-linux-uclibcgnueabihf` (GCC 8.3) | Its `bin/` must be on `PATH` |
| `cmake` ≥ 3.20, `ninja`, `git` | Only for building libmoq |
| Network access | The first libmoq build clones the pinned picoquic and picotls into `../.deps/rv1106` |
| OpenSSL 1.1 shared libs of the board | See below |

### OpenSSL

The app links dynamically against the board's OpenSSL, which the rootfs already ships. Git keeps only the headers. Copy the two libraries from the board SDK, or from the board itself, and create the link-time symlinks:

```sh
cd ipc-media/3rd-parties/libopenssl-1.1.1h
mkdir -p lib
cp <board-sdk>/.../libssl.so.1.1 <board-sdk>/.../libcrypto.so.1.1 lib/
ln -sf libssl.so.1.1 lib/libssl.so
ln -sf libcrypto.so.1.1 lib/libcrypto.so
```

The board must provide the same sonames: `libssl.so.1.1` and `libcrypto.so.1.1`. If the board has OpenSSL 3 (`libssl.so.3`), you must rebuild both libmoq and the app against OpenSSL 3.

## 1. Build libmoq

```sh
export PATH=<toolchains>/arm-rockchip830-linux-uclibcgnueabihf/bin:$PATH
ipc-media/3rd-parties/libmoq5/build.sh
```

The script does the following:
- Fetches and patches picoquic and picotls into `.deps/rv1106/`.
- Configures libmoq in `build-rv1106/` (MinSizeRel). It enables the service tier, the raw-QUIC picoquic adapter, the CMAF packager and `MOQ_CATALOG_INLINE_INIT_DATA`. WebTransport, tests and examples are disabled.
- Installs headers and static archives into `ipc-media/3rd-parties/libmoq5/{include,lib}`.

You only need to rerun it when libmoq changes, for example after editing files under `core/`, `service/`, `media/` or `adapters/`.

## 2. Build the app

```sh
cd ipc-media
make -j"$(nproc)"
```

The output is `build/ipc-media`, stripped, about 1.1 MB. To use a different toolchain prefix, run `make CROSS_COMPILER=<prefix>`.

## Configuration

The settings are compiled in: edit `sDefautlConfiguration` at the top of `main.cpp` and rebuild.

| Field | Meaning |
|---|---|
| `stVideoPath`, `stAudioPath` | Ring buffers written by the encoder |
| `sMediaConfigs.vCodec` | `"H264"` or `"H265"`: which video encoding is published (frames of the other codec are dropped) |
| `sMediaConfigs.aSamplesrate`, `aNumChannels` | Must match the PCM/G.711 input |
| `mute` | `true` publishes video only |
| `stEndpoint` | Relay URL, `moqt://host:port` |
| `stNamespace` | Namespace the viewer subscribes to |
| `tlsSupport`, `certificate` | Verify the relay certificate against this CA file (must be non-empty when enabled) |
| `reconnectAfterSecs` | Delay before reconnecting after a session ends |

## Run and view

Copy `build/ipc-media` to the board and start it after the encoder is running. A healthy start logs lines like these:

```
[MOQ] Video avc1.640028 1280x720
[MOQ] Connecting moqt://relay.moqtail.dev:443 as livestream (avc1.640028) ...
[MOQ] Connected moqt://relay.moqtail.dev:443 (draft 22), publishing livestream
[MOQ] Stats: written=... sent=... queued=0 dropped=0 last_error=0
```

Errors are printed as `[MOQ][ERROR] ...` lines. They come from the publisher's error callback and include the libmoq result code.

To watch the stream, open <https://player.moqtail.dev>, go to the **Watch** tab, and set:
- Relay: `moqt://relay.moqtail.dev`
- Namespace: the value of `stNamespace`

H.265 plays only in browsers with HEVC support in Media Source Extensions (Safari, or Chrome/Edge with hardware decoding).

## Clean

| What | Command |
|---|---|
| App objects and binary | `make -C ipc-media clean` |
| libmoq build tree | `rm -rf build-rv1106` |
| Installed libmoq (rerun `build.sh` afterwards) | `rm -rf ipc-media/3rd-parties/libmoq5/{include,lib,share}` |
| Fetched picoquic/picotls (the next `build.sh` clones them again) | `rm -rf .deps/rv1106` |

All paths are relative to the repository root. None of these outputs are tracked by git.
