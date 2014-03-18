#!/bin/bash
MODULE=$1

UDEV_RULE=/etc/udev/rules.d/41-${MODULE}.rules

if [[ `id -u` != 0 ]]
then
  echo "Must be run as root"
  exit 1
fi

TARGET=${MODULE}.ko

INSTALL_PATH=/lib/modules/`uname -r`/extra

INSTALLED_TARGET=`find ${INSTALL_PATH} -name ${TARGET}`
if [[ -e ${INSTALLED_TARGET} ]]
then
  echo "Removing installed module" ${INSTALLED_TARGET}
  rm ${INSTALLED_TARGET}
fi

if [[ -e ${UDEV_RULE} ]]
then
  echo "removing udev rule" ${UDEV_RULE}
  rm ${UDEV_RULE}
fi

echo "depmod -a"
depmod -a
