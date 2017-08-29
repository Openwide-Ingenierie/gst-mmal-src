#! /bin/bash

# Common env.
script_path="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${script_path}/env.sh"

# ----------------------------------------
# GSTREAMER 1.8 COMPILATION & INSTALL
# ----------------------------------------

src_path="${HOME}/gstreamer"
install_path="${HOME}/gstreamer/install"
mkdir -p "${src_path}"
mkdir -p "${install_path}"

function install_gst()
{
    # Arg 1 is the name of the module
    cd "${src_path}"
    echo "Installing ${1}"
        if [ ! -e "${1}" ]; then
        git clone git://anongit.freedesktop.org/gstreamer/${1}
        cd "${1}"
        git checkout 1.8
        ./autogen.sh
        ./configure --prefix="${install_path}/usr/local"
        make
        make install
    fi
}

install_gst gstreamer
install_gst gst-plugins-base
install_gst gst-plugins-good
install_gst gst-plugins-bad
install_gst gst-plugins-ugly
install_gst gst-libav

exit 0
