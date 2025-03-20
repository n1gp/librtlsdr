# Copyright 2012 OSMOCOM Project
#
# This file is part of rtl-sdr
#
# GNU Radio is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3, or (at your option)
# any later version.
#
# GNU Radio is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with GNU Radio; see the file COPYING.  If not, write to
# the Free Software Foundation, Inc., 51 Franklin Street,
# Boston, MA 02110-1301, USA.

MACRO(RTLSDR_APPEND_SRCS)
    LIST(APPEND rtlsdr_srcs ${ARGV})
ENDMACRO(RTLSDR_APPEND_SRCS)

RTLSDR_APPEND_SRCS(
    librtlsdr.c
    tuner_e4k.c
    tuner_fc0012.c
    tuner_fc0013.c
    tuner_fc2580.c
    tuner_r82xx.c
)

########################################################################
# Set up Windows DLL resource files
########################################################################
IF(MSVC)
    include(${PROJECT_SOURCE_DIR}/cmake/Modules/Version.cmake)

    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/rtlsdr.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/rtlsdr.rc
    @ONLY)

    RTLSDR_APPEND_SRCS(${CMAKE_CURRENT_BINARY_DIR}/rtlsdr.rc)
ENDIF(MSVC)

########################################################################
# Setup shared library variant
########################################################################
add_library(rtlsdr_shared SHARED ${rtlsdr_srcs})
if(NOT WIN32)
    target_link_libraries(rtlsdr_shared ${LIBUSB_LIBRARIES})
else()
    target_link_libraries(rtlsdr_shared ws2_32 ${LIBUSB_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
endif()

set_target_properties(rtlsdr_shared PROPERTIES DEFINE_SYMBOL "rtlsdr_EXPORTS")
set_target_properties(rtlsdr_shared PROPERTIES OUTPUT_NAME rtlsdr)
set_target_properties(rtlsdr_shared PROPERTIES SOVERSION ${MAJOR_VERSION})
set_target_properties(rtlsdr_shared PROPERTIES VERSION ${LIBVER})

########################################################################
# Setup static library variant
########################################################################
add_library(rtlsdr_static STATIC ${rtlsdr_srcs})
if(NOT WIN32)
    target_link_libraries(rtlsdr_static ${LIBUSB_LIBRARIES})
else()
    target_link_libraries(rtlsdr_static ws2_32 ${LIBUSB_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
endif()

set_property(TARGET rtlsdr_static APPEND PROPERTY COMPILE_DEFINITIONS "rtlsdr_STATIC" )
if(NOT WIN32)
    # Force same library filename for static and shared variants of the library
    set_target_properties(rtlsdr_static PROPERTIES OUTPUT_NAME rtlsdr)
endif()


########################################################################
# Set link library
########################################################################

if(LINK_RTLTOOLS_AGAINST_STATIC_LIB)
    set(RTLSDR_TOOL_LIB rtlsdr_static)
else()
    set(RTLSDR_TOOL_LIB rtlsdr_shared)
endif()

########################################################################
# Setup libraries used in executables
########################################################################
add_library(convenience_static STATIC
    convenience/convenience.c
)

if(WIN32)
add_library(libgetopt_static STATIC
    getopt/getopt.c
)
target_link_libraries(convenience_static
    ${RTLSDR_TOOL_LIB}
)
endif()

########################################################################
# Build utility
########################################################################
add_executable(rtl_sdr rtl_sdr.c)
add_executable(rtl_tcp rtl_tcp.c)
if(WIN32)
add_executable(rtl_hpsdr rtl_hpsdr.c downsample.c)
else()
add_executable(rtl_hpsdr rtl_hpsdr.c downsample.c local_sound.c)
endif()
add_executable(rtl_test rtl_test.c)
add_executable(rtl_fm rtl_fm.c)
add_executable(rtl_eeprom rtl_eeprom.c)
add_executable(rtl_adsb rtl_adsb.c)
add_executable(rtl_power rtl_power.c)
add_executable(rtl_biast rtl_biast.c)

set(INSTALL_TARGETS rtlsdr_shared rtlsdr_static rtl_sdr rtl_tcp rtl_hpsdr rtl_test rtl_fm rtl_eeprom rtl_adsb rtl_power rtl_biast)


target_link_libraries(rtl_sdr ${RTLSDR_TOOL_LIB} convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_tcp ${RTLSDR_TOOL_LIB} convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_hpsdr ${RTLSDR_TOOL_LIB} convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${FFTW3_LIBRARIES}
)
target_link_libraries(rtl_test ${RTLSDR_TOOL_LIB} convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_fm ${RTLSDR_TOOL_LIB} convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_eeprom ${RTLSDR_TOOL_LIB} convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_adsb ${RTLSDR_TOOL_LIB} convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_power ${RTLSDR_TOOL_LIB} convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
target_link_libraries(rtl_biast ${RTLSDR_TOOL_LIB} convenience_static
    ${LIBUSB_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)

if(UNIX)
    target_link_libraries(rtl_fm m)
    target_link_libraries(rtl_adsb m)
    target_link_libraries(rtl_power m)
	if(WIN32)
		target_link_libraries(rtl_hpsdr fftw3 m)
	else()
		target_link_libraries(rtl_hpsdr asound fftw3 m)
	endif()
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
    find_library(LIBM NAMES m)
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

# check for SSE extensions
include(CheckCSourceRuns)
if( CMAKE_COMPILER_IS_GNUCC AND NOT WIN32 )
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
install(TARGETS ${INSTALL_TARGETS}
    LIBRARY DESTINATION ${LIB_INSTALL_DIR} # .so/.dylib file
    ARCHIVE DESTINATION ${LIB_INSTALL_DIR} # .lib file
    RUNTIME DESTINATION bin              # .dll file
)
