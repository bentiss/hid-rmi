#!/bin/bash
MODULE=$1
BLACKLIST_MODULE=$2

UDEV_RULE=/etc/udev/rules.d/99-${MODULE}.rules
LOADING_SCRIPT=load_hid_specific_module.sh
BASH_LOADING_SCRIPT=/etc/udev/${LOADING_SCRIPT}

if [[ `id -u` != 0 ]]
then
  echo "Must be run as root"
  exit 1
fi


echo 'ACTION!="add|change", GOTO="hid_rmi_end"' > ${UDEV_RULE}
grep MODULE_ALIAS ${MODULE}.mod.c | \
	sed "s/MODULE_ALIAS(\"\(hid:b.*g.*v.*p.*\)\");/DRIVER==\"${BLACKLIST_MODULE}\", ENV{MODALIAS}==\"\1\", RUN+=\"\/bin\/sh \/etc\/udev\/${LOADING_SCRIPT} ${BLACKLIST_MODULE} ${MODULE} %k\"/" | \
	grep -v MODULE_ALIAS | \
	sort \
		>> ${UDEV_RULE}
echo 'LABEL="hid_rmi_end"' >> ${UDEV_RULE}

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
