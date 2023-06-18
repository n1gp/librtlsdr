rtl-sdr
turns your Realtek RTL2832 based DVB dongle into a SDR receiver
======================================================================

Dockerized version of the library for use with cw-skimmer docker container.


# Watch out!! CABLE
- The cables delivered with the RTL SDR devices are crap. Get new ones or you will have an USB error.

# Watch out: blacklist this module:
dvb_usb_rtl28xxu
sudo echo blacklist dvb_usb_rtl28xxu > /etc/modprobe.d/blacklist-dvb_usb_rtl28xxu.conf