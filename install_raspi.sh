#!/bin/bash

sudo apt-get update 
sudo apt-get -y upgrade
sudo apt-get install -y build-essential git cmake libfftw3-dev libusb-1.0-0-dev libfftw3-3 libusb-dev pkgconf libasound2-dev

mkdir build
cd build 
cmake -DCMAKE_C_FLAGS=-fcommon  ..
make 
make install