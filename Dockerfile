FROM debian

RUN apt-get update && apt-get -y upgrade
RUN apt-get install -y build-essential git cmake libfftw3-dev libusb-1.0-0-dev libfftw3-3 libusb-dev pkgconf libasound2-dev
RUN apt-get -qqy autoclean && rm -rf /tmp/* /var/tmp/*


WORKDIR /app/src
RUN git clone https://github.com/n1gp/librtlsdr.git
WORKDIR /app/src/librtlsdr/build
RUN cmake -DCMAKE_C_FLAGS=-fcommon  ..
RUN make 
RUN make install


WORKDIR /app
ADD ./conf/rtl_hpsdr.conf /app/
ADD ./conf/startup.sh /app
ENV LD_LIBRARY_PATH=/usr/local/lib

#CMD ["rtl_hpsdr", "-c", "/app/rtl_hpsdr.conf"]
CMD ["/app/startup.sh"]
