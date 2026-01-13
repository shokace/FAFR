# FAFR FFmpeg Patch (7.0.x)

This folder contains a patch that adds a FAFR demuxer + decoder to FFmpeg.

## Apply

```
cd /ffmpeg
git checkout release/7.0
git apply /FAFR/ffmpeg_patches/0001-add-fafr-demuxer-decoder.patch
```

## Build (example)

```
./configure --enable-demuxer=fafr --enable-decoder=fafr
make -j
```

## Notes

- The demuxer expects `.fafr` files with the 40-byte header described in W2F.
- The decoder outputs packed float PCM (`AV_SAMPLE_FMT_FLT`) at the file sample rate.
