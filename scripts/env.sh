export LD_LIBRARY_PATH="${HOME}/gstreamer/install/usr/local/lib:${HOME}/gstreamer/install/usr/lib"
export PKG_CONFIG_PATH="${HOME}/gstreamer/install/usr/local/lib/pkgconfig"

if ! echo "${PATH}" | grep -q ":${HOME}/gstreamer/install/usr/bin"; then
    export PATH="${HOME}/gstreamer/install/usr/bin:${PATH}"
fi
if ! echo "${PATH}" | grep -q ":${HOME}/gstreamer/install/usr/local/bin"; then
    export PATH="${HOME}/gstreamer/install/usr/local/bin:${PATH}"
fi

export GST_PATH="${HOME}/gstreamer/install"
