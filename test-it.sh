#!/bin/sh

MODS="cs128p cs256pp blake3"

for m in $MODS; do
  echo sudo rmmod poc_${m}
  sudo rmmod poc_${m}
done

for m in $MODS; do
  echo sudo insmod ./poc_${m}.ko
  sudo insmod ./poc_${m}.ko
done

for m in $MODS; do
  echo dd if=/dev/urandom-${m} of=/tmp/${m} bs=1M count=1k
  dd if=/dev/urandom-${m} of=/tmp/${m} bs=1M count=1k
done
