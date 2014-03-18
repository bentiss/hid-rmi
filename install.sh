#!/bin/bash
MODULE=$1
BLACKLIST_MODULE=$2

UDEV_RULE=/etc/udev/rules.d/41-${MODULE}.rules
LOADING_SCRIPT=load_hid_specific_module.sh
BASH_LOADING_SCRIPT=/etc/udev/${LOADING_SCRIPT}

if [[ `id -u` != 0 ]]
then
  echo "Must be run as root"
  exit 1
fi

grep MODULE_ALIAS hid_mt_compat.mod.c | \
	sed 's/MODULE_ALIAS("hid:b.*v0000\(.*\)p0000\(.*\)");/DRIVER=="${BLACKLIST_MODULE}", ENV{MODALIAS}=="usb:v\1p\2d*", RUN+="\/bin\/sh \/etc\/udev\/${LOADING_SCRIPT} ${BLACKLIST_MODULE} ${MODULE} %k"/' | \
	grep -v MODULE_ALIAS | \
	sort \
		> ${UDEV_RULE}

cat > ${BASH_LOADING_SCRIPT} <<EOF
#!/bin/bash

BLACKLIST_DRIVER=\$1
NEW_DRIVER=\$2
DEVICE=\$3

HID_DRV_PATH=/sys/bus/hid/drivers

/sbin/modprobe \${NEW_DRIVER}

echo \${DEVICE} > \${HID_DRV_PATH}/\${BLACKLIST_DRIVER}/unbind
echo \${DEVICE} > \${HID_DRV_PATH}/\${NEW_DRIVER}/bind
EOF
