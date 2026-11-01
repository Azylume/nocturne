# Test media

Most clips here are made by `make_media.sh`, `make_hap_goldens.sh` and `unusual_media.sh`; their headers say how.

## return_video_426.bin

Ten return video packets (key frame first) of the test card `card(426, 240, i)` from `tests/ffmpeg_movies.hpp`,
encoded by Nocturne's own off-Apple `H264Encoder` at the picture size and rate of a 640x360 composition's return video
with one output (426x240), at 40 kbit/s so that the file stays near 5 KB. The file layout is in `tests/return_video_packets.hpp`. The test "off Apple, the H.264 decoder returns
426-wide pictures without writing past them" (`tests/return_video_encode_test.cpp`) decodes it, so that it runs on
machines with no H.264 encoder that hands back packets at once.

It was made on Windows (RTX 3050 Ti Laptop, NVENC through the bundled FFmpeg), from a Release build, in the logged-in
desktop session, from the source root:

```bat
set NOCTURNE_RETURN_VIDEO_FIXTURE_OUT=tests\media
build\nocturne_tests.exe -tc="*the H.264 decoder returns 426*"
```

## return_video_bars.bin

Made on a Mac; the comment above "return video: packets a Mac encoded decode to their picture on every platform"
(`tests/h264_decoder_test.cpp`) says how.
