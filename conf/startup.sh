#!/bin/bash
IP=$(cat /etc/hosts |tail -n1|sed 's/[ \t].*//g')
rtl_hpsdr -i $IP -d 3