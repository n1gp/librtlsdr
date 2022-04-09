#!/bin/bash

sudo apt-get update 
sudo apt-get -y upgrade
sudo apt-get install -y build-essential git cmake libfftw3-dev libusb-1.0-0-dev libfftw3-3 libusb-dev pkgconf libasound2-dev

mkdir src
cd src
RUN git clone https://github.com/n1gp/librtlsdr.git
cd librtlsdr
mkdir build
cd build 
RUN cmake -DCMAKE_C_FLAGS=-fcommon  ..
RUN make 
RUN make install