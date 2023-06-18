rtl-sdr
turns your Realtek RTL2832 based DVB dongle into a SDR receiver
======================================================================

For more information see:
http://sdr.osmocom.org/trac/wiki/rtl-sdr

This repository https://github.com/n1gp/librtlsdr was forked from
https://github.com/steve-m/librtlsdr. It's purpose is to add the
rtl_hpsdr application and allows building it for Windows or Linux.

See http://www.staniscia.net/1023-how-to-build-rtl-sdr-on-windows-x64
for an example on how to build on Windows.

For Linux:

mkdir build
cd build
cmake ..
make


# WIP - Config

## Kernel module
 
/etc/modprobe.d/rtlsdr.conf
blacklist dvb_usb_rtl28xxu


The important parts are "0bda" (the vendor id) and "2838" (the product id).

Create a new file as root named /etc/udev/rules.d/20.rtlsdr.rules that contains the following line:

SUBSYSTEM=="usb", ATTRS{idVendor}=="0bda", ATTRS{idProduct}=="2838", GROUP="adm", MODE="0666", SYMLINK+="rtl_sdr"

## Direct Sampling for HF

rtl_eeprom 
Found 1 device(s):
  0:  Generic RTL2832U OEM

Using device 0: Generic RTL2832U OEM
Found Rafael Micro R828D tuner

Current configuration:
__________________________________________
Vendor ID:		0x0bda
Product ID:		0x2838
Manufacturer:		Realtek
Product:		RTL2838UHIDIR
Serial number:		00000001
Serial number enabled:	yes
IR endpoint enabled:	yes
Remote wakeup enabled:	no
__________________________________________



Forcing Direct Sampling To be Always ON

In some cases you may want to force direct sampling to be always on. For example, not all programs expose the direct sampling controls to the user, so in those programs it can be impossible to turn direct sampling mode on. To force it on, use our RTL-SDR-Blog driver branch, and use set the direct sampling always on flag in the EEPROM with the command "rtl_eeprom -q y". To disable forced direct sampling do the opposite command "rtl_eeprom -q n"




./src/rtl_eeprom -q y
Found 1 device(s):
  0:  Generic RTL2832U OEM

Using device 0: Generic RTL2832U OEM
Found Rafael Micro R828D tuner

Current configuration:
__________________________________________
Vendor ID:		0x0bda
Product ID:		0x2838
Manufacturer:		Realtek
Product:		RTL2838UHIDIR
Serial number:		00000001
Serial number enabled:	yes
Bias-T Always ON:	no
Direct Sampling Always ON:	no
__________________________________________

New configuration:
__________________________________________
Vendor ID:		0x0bda
Product ID:		0x2838
Manufacturer:		Realtek
Product:		RTL2838UHIDIR
Serial number:		00000001
Serial number enabled:	yes
Bias-T Always ON:	no
Direct Sampling Always ON:	no
__________________________________________
Write new configuration to device [y/n]? y

Configuration successfully written.
Please replug the device for changes to take effect.



## Configuration of local IP and container
...

## Link with docker-cwskimmer configuration
...

## Starting in container:
he standard Osmocom version of rtl_tcp only allows for direct sampling on the I-branch, which is useless as we need direct sampling on the Q-branch. Please see our RTL-SDR-Blog Drivers for a version that includes a -D direct sampling flag. The Releases page has a Windows release.

./src/librtlsdr/build/src/rtl_hpsdr -i 172.17.0.3 -d 2

better after configured once 

./src/librtlsdr/build/src/rtl_hpsdr -i 172.17.0.3 -d 3

## Building
+ make -B -f Makefile.docker build     