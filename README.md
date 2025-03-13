# gstjitsimeet
Jitsi Meet GStreamer plugin  
# Dependencies
* gstreamer >= 1.20
* gst-plugins-good
* gst-plugins-bad
* libnice
* meson
* OpenSSL
* libwebsockets

Ubuntu:
```
apt install \
gstreamer1.0-libav \
gstreamer1.0-nice \
gstreamer1.0-plugins-rtp \
gstreamer1.0-plugins-ugly \
libgstreamer-plugins-bad1.0-dev \
libgstreamer-plugins-base1.0-dev \
libgstreamer-plugins-good1.0-dev \
libgstreamer1.0-dev \
libnice-dev \
meson \
libssl-dev \
libwebsockets-dev
```
Gentoo:
```
emerge \
media-libs/gst-plugins-bad \
media-libs/gst-plugins-base \
media-libs/gst-plugins-good \
media-libs/gstreamer \
media-plugins/gst-plugins-dtls \
media-plugins/gst-plugins-libav \
media-plugins/gst-plugins-libnice \
media-plugins/gst-plugins-opus \
media-plugins/gst-plugins-pulse \
media-plugins/gst-plugins-srtp \
media-plugins/gst-plugins-x264 \
net-libs/libwebsockets
```

# Build
```
# install dependent library
git clone https://github.com/mojyack/coop.git
pushd coop
meson setup build --prefix=/tmp/rootfs
ninja -C build install
export PKG_CONFIG_PATH=/tmp/rootfs/lib/pkgconfig
popd
# build gstjitsimeet
git clone --recursive https://github.com/mojyack/gstjitsimeet.git
pushd gstjitsimeet
meson setup build --buildtype=release
ninja -C build
```

# Example
Streaming:
```
export GST_PLUGIN_PATH=$PWD/build
gst-launch-1.0 videotestsrc ! videoconvert ! video/x-raw,width=300,height=200 ! x264enc ! .video_sink jitsibin room=example server=jitsi.example room=example
```
Streaming with audio:
```
export GST_PLUGIN_PATH=$PWD/build
pipeline=(
    jitsibin
        name=room
        server=jitsi.example
        room=example
    videotestsrc is-live=true !
    videoconvert !
    video/x-raw,width=300,height=200 !
    x264enc !
    room.video_sink

    audiotestsrc is-live=true wave=8 !
    audioconvert !
    opusenc !
    room.audio_sink
)

gst-launch-1.0 $pipeline
```
Receiving is a little more complicated because you have to handle signals  
See examples in `src/examples`
# Credits
MUC initialize sequences are taken from [avstack/gst-meet](https://github.com/avstack/gst-meet)
