# Copyright 2012-2020 Osmocom Project
#
# This file is part of rtl-sdr
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

########################################################################
# Setup shared library variant
########################################################################
add_library(rtlsdr SHARED librtlsdr.c
  tuner_e4k.c tuner_fc0012.c tuner_fc0013.c tuner_fc2580.c tuner_r82xx.c)
target_link_libraries(rtlsdr ${LIBUSB_LIBRARIES} ${THREADS_PTHREADS_LIBRARY})
target_include_directories(rtlsdr PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>  # <prefix>/include
  ${LIBUSB_INCLUDE_DIRS}
  ${THREADS_PTHREADS_INCLUDE_DIR}
  )
set_target_properties(rtlsdr PROPERTIES DEFINE_SYMBOL "rtlsdr_EXPORTS")
set_target_properties(rtlsdr PROPERTIES OUTPUT_NAME rtlsdr)
set_target_properties(rtlsdr PROPERTIES SOVERSION ${MAJOR_VERSION})
set_target_properties(rtlsdr PROPERTIES VERSION ${LIBVER})
generate_export_header(rtlsdr)

########################################################################
# Setup static library variant
########################################################################
add_library(rtlsdr_static STATIC librtlsdr.c
  tuner_e4k.c tuner_fc0012.c tuner_fc0013.c tuner_fc2580.c tuner_r82xx.c)
target_link_libraries(rtlsdr ${LIBUSB_LIBRARIES} ${THREADS_PTHREADS_LIBRARY})
target_include_directories(rtlsdr_static PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>  # <prefix>/include
  ${LIBUSB_INCLUDE_DIRS}
  ${THREADS_PTHREADS_INCLUDE_DIR}
  )
set_property(TARGET rtlsdr_static APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
if(NOT WIN32)
# Force same library filename for static and shared variants of the library
set_target_properties(rtlsdr_static PROPERTIES OUTPUT_NAME rtlsdr)
endif()
generate_export_header(rtlsdr_static)

########################################################################
# Set up Windows DLL resource files
########################################################################
IF(MSVC)
    include(${CMAKE_SOURCE_DIR}/cmake/Modules/Version.cmake)

    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/rtlsdr.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/rtlsdr.rc
    @ONLY)
  target_sources(rtlsdr PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/rtlsdr.rc)
  target_sources(rtlsdr_static PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/rtlsdr.rc)
ENDIF(MSVC)

########################################################################
# Setup libraries used in executables
########################################################################
add_library(convenience_static STATIC
    convenience/convenience.c
)
target_include_directories(convenience_static
  PRIVATE ${CMAKE_SOURCE_DIR}/include)
if(WIN32)
add_library(libgetopt_static STATIC
    getopt/getopt.c
)
target_link_libraries(convenience_static
    rtlsdr
)
endif()

########################################################################
# Build utility
########################################################################
add_executable(rtl_sdr rtl_sdr.c)
add_executable(rtl_tcp rtl_tcp.c)
add_executable(rtl_test rtl_test.c)
add_executable(rtl_fm rtl_fm.c)
add_executable(rtl_eeprom rtl_eeprom.c)
add_executable(rtl_adsb rtl_adsb.c)
add_executable(rtl_power rtl_power.c)
add_executable(rtl_biast rtl_biast.c)
add_executable(rtl_hpsdr rtl_hpsdr.c downsample.c local_sound.c)
set(INSTALL_TARGETS rtlsdr rtlsdr_static rtl_sdr rtl_tcp rtl_hpsdr rtl_test rtl_fm rtl_eeprom rtl_adsb rtl_power rtl_biast)

target_link_libraries(rtl_sdr rtlsdr convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_tcp rtlsdr convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_hpsdr rtlsdr convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${FFTW3_LIBRARIES}
)
target_link_libraries(rtl_test rtlsdr convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_fm rtlsdr convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_eeprom rtlsdr convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_adsb rtlsdr convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_power rtlsdr convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_biast rtlsdr convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
if(UNIX)
target_link_libraries(rtl_fm m)
target_link_libraries(rtl_adsb m)
target_link_libraries(rtl_power m)
target_link_libraries(rtl_hpsdr asound fftw3 m)
if(APPLE OR CMAKE_SYSTEM MATCHES "OpenBSD")
    target_link_libraries(rtl_test m)
else()
    target_link_libraries(rtl_test m rt)
endif()
endif()

if(WIN32)
target_link_libraries(rtl_sdr libgetopt_static)
target_link_libraries(rtl_tcp ws2_32 libgetopt_static)
target_link_libraries(rtl_hpsdr ws2_32 iphlpapi libgetopt_static)
target_link_libraries(rtl_test libgetopt_static)
target_link_libraries(rtl_fm libgetopt_static)
target_link_libraries(rtl_eeprom libgetopt_static)
target_link_libraries(rtl_adsb libgetopt_static)
target_link_libraries(rtl_power libgetopt_static)
target_link_libraries(rtl_biast libgetopt_static)
set_property(TARGET rtl_sdr APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
set_property(TARGET rtl_tcp APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
set_property(TARGET rtl_hpsdr APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
set_property(TARGET rtl_test APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
set_property(TARGET rtl_fm APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
set_property(TARGET rtl_eeprom APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
set_property(TARGET rtl_adsb APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
set_property(TARGET rtl_power APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
set_property(TARGET rtl_biast APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
endif()
# add SSE2 optimization (if available) for rtl_hpsdr
#if (CMAKE_COMPILER_IS_GNUCC AND (CMAKE_SYSTEM_PROCESSOR MATCHES ".*64"
#	OR CMAKE_SYSTEM_PROCESSOR MATCHES ".*i686"))
#	set_property(TARGET rtl_hpsdr APPEND PROPERTY COMPILE_FLAGS "-DINCLUDE_SSE2 -msse2" )
#endif()

# check for SSE extensions
include(CheckCSourceRuns)
if( CMAKE_COMPILER_IS_GNUCC )
	set(INTRIN_FLAGS)
  
	set(CMAKE_REQUIRED_FLAGS "-msse3")
	check_c_source_runs("
	#include <pmmintrin.h>
  
	int main()
	{
		__m128d a, b;
		double vals[2] = {0};
		a = _mm_loadu_pd(vals);
		b = _mm_hadd_pd(a,a);
		_mm_storeu_pd(vals, b);
		return 0;
	}"
	HAS_SSE3_EXTENSIONS)
  
	set(CMAKE_REQUIRED_FLAGS "-msse2")
	check_c_source_runs("
	#include <emmintrin.h>
  
	int main()
	{
		 __m128d a, b;
		 double vals[2] = {0};
		 a = _mm_loadu_pd(vals);
		 b = _mm_add_pd(a,a);
		 _mm_storeu_pd(vals,b);
		 return 0;
	}"
	HAS_SSE2_EXTENSIONS)
  
	set(CMAKE_REQUIRED_FLAGS)
  
	if(HAS_SSE3_EXTENSIONS OR HAS_SSE2_EXTENSIONS)
		set(INTRIN_FLAGS "-DINCLUDE_SSE2 -msse2 -mfpmath=sse")
		set_property(TARGET rtl_hpsdr APPEND PROPERTY COMPILE_FLAGS ${INTRIN_FLAGS} )
		message(STATUS "Found SSE2 extensions, using flags: ${INTRIN_FLAGS}")
	elseif(${CMAKE_SYSTEM_PROCESSOR} MATCHES aarch64)
		set(INTRIN_FLAGS "-DINCLUDE_NEON -O3 -ffast-math -funroll-loops -ftree-vectorize -march=armv8-a+fp+simd")
		#set(INTRIN_FLAGS "-DINCLUDE_NEON -O3 -funroll-loops -march=native")
		set_property(TARGET rtl_hpsdr APPEND PROPERTY COMPILE_FLAGS ${INTRIN_FLAGS} )
		message(STATUS "Found ARM aarch64 NEON extensions, using flags: ${INTRIN_FLAGS}")
	elseif(${CMAKE_SYSTEM_PROCESSOR} MATCHES armv7*)
		set(INTRIN_FLAGS "-DINCLUDE_NEON -O3 -funroll-loops -march=armv7-a -mfloat-abi=hard -mfpu=neon")
		set_property(TARGET rtl_hpsdr APPEND PROPERTY COMPILE_FLAGS ${INTRIN_FLAGS} )
		message(STATUS "Found ARM NEON extensions, using flags: ${INTRIN_FLAGS}")
	endif()
endif()

PROJECT(VERSIONHEADER C)
FIND_PACKAGE(Git)
FILE(WRITE ${CMAKE_BINARY_DIR}/version.h.in
"\#define GITVERSION \"@GIT_COMMIT_HASH@\"\n"
)
FILE(WRITE ${CMAKE_BINARY_DIR}/version.cmake
"EXECUTE_PROCESS(
	COMMAND git log -1 --format=%h
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	OUTPUT_VARIABLE GIT_COMMIT_HASH
	OUTPUT_STRIP_TRAILING_WHITESPACE
 )
 CONFIGURE_FILE(\${SRC} \${DST} @ONLY)
")
INCLUDE_DIRECTORIES(${CMAKE_BINARY_DIR})
#ADD_EXECUTABLE(main main.c)
ADD_CUSTOM_TARGET(
	version
	${CMAKE_COMMAND} -D SRC=${CMAKE_BINARY_DIR}/version.h.in
	-D DST=${CMAKE_BINARY_DIR}/version.h
	-P ${CMAKE_BINARY_DIR}/version.cmake
)
ADD_DEPENDENCIES(rtl_hpsdr version)
########################################################################
# Install built library files & utilities
########################################################################
install(TARGETS rtlsdr EXPORT RTLSDR-export
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} # .so/.dylib file
  )
install(TARGETS rtlsdr_static EXPORT RTLSDR-export
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} # .so/.dylib file
  )
install(TARGETS rtl_sdr rtl_tcp rtl_test rtl_fm rtl_eeprom rtl_adsb rtl_power rtl_biast
  DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
