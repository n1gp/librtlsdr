#!/bin/bash

freq=125000000	#upconvertor center freq
width=5000	#gap +- around center freq
itime=10	#iteration time for rtl_power
ntime=20	#length of each dongle cal
warmup=30	#warmup time to bring dongles to operating temp
xtal=-450	#how far off your upconvertor xtal is
fudge0=850	#fudge factor for problem child dongle 0
crop=80

count=`rtl_eeprom 2>&1 | egrep 'Found [0-9]' | cut -d' ' -f2`

killall -9 rtl_power > /dev/null 2>&1

echo "Found  a total of $count dongles."
lcount=$(( count / 2 ))
zcount=$(( count - lcount ))
count=$(( count  - 1 ))

echo "Warming up for $warmup seconds."
for i in `seq 0 $count`; do
    rtl_test -d $i -b 8192 -S -s 250001 > /dev/null 2>&1 &
    pid_array[$i]=$!
done
sleep $warmup

for i in `seq 0 $count`; do
    kill ${pid_array[$i]} > /dev/null 2>&1
done

echo "Starting calibration on first $lcount dongles, this will take $ntime seconds."

fcount=$(( lcount - 1 ))
fstart=0

for i in `seq $fstart $fcount`; do
    #echo "spawning $i"
    rtl_power -F 9 -d $i -f $(( freq - width )):$(( freq + width )):1 -r 2048000 \
        -o $freq -c $crop /dev/shm/rtl_$i -i $itime -e $ntime > /dev/null 2>&1 &
done

mtime=$ntime
while [ $mtime -ne 0 ]; do
    echo $mtime left
    sleep 10
    mtime=$(( mtime - 10 ))
done

while true; do
    ps -Af|grep ' rtl_power'|grep -v grep > /dev/null 2>&1
    [ $? -ne 0 ] && break
    sleep 1
    #echo "Waiting for rtl_power to stop..."
done

fstart=$lcount

echo "Starting calibration on remaining $zcount dongles, this will take $ntime seconds."

for i in `seq $fstart $count`; do
    #echo "spawning $i"
    rtl_power -F 9 -d $i -f $(( freq - width )):$(( freq + width )):1 -r 2048000 \
        -o $freq -c $crop /dev/shm/rtl_$i -i $itime -e $ntime > /dev/null 2>&1 &
done

mtime=$ntime
while [ $mtime -ne 0 ]; do
    echo $mtime left
    sleep 10
    mtime=$(( mtime - 10 ))
done

echo "Calculating calibration values for dongles 0 - $count ..."
while true; do
    ps -Af|grep ' rtl_power'|grep -v grep > /dev/null 2>&1
    [ $? -ne 0 ] && break
    sleep 1
    #echo "Waiting for rtl_power to stop..."
done

echo -n "freq_offset "
for i in `seq 0 $count`; do
    offset=0
    loop=0
    while read line; do           
        offset=$(( line + offset ))
        loop=$(( loop + 1 ))
    done <  <(cat /dev/shm/rtl_$i | grep "offset =" | cut -d' ' -f13)

    offset=$(( offset / loop ))
    [ $i -eq 0 ] && offset=$(( offset + fudge0 ))

    echo -n "$(( freq + offset + xtal ))"
    [ $i -ne $count ] && echo -n ","
done
echo
