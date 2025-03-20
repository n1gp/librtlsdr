rtl-sdr
turns your Realtek RTL2832 based DVB dongle into a SDR receiver

LINUX:
cd build
cmake ..
make -j4
sudo make install

rtl_hpsdr -h   (for help)



WINDOWS (built on Linux using MingW)

32-bit:  ./cross_build_mingw32.sh output

Binaries are in rtlsdr-bin-w32_output/bin


64-bit:  ./cross_build_mingw64.sh output

Binaries are in rtlsdr-bin-w64_output/bin

======================================================================

Dockerized version of the library for use with cw-skimmer docker container.

# Watch out!! CABLE
- The cables delivered with the RTL SDR devices are crap. Get new ones or you will have an USB error.

# Watch out: blacklist this module:
dvb_usb_rtl28xxu
sudo install-blacklist.sh
