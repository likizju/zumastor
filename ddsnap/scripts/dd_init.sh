#!/bin/bash 

# import all the sweet variables used throughout the ddsnap scripts
. ddsnap_env.sh

cd $DDSNAP_HOME

/sbin/dmsetup remove_all || true
killall ddsnap

echo "Initializing devices locally and remotely"
./ddsnap initialize $SNAPSTORE_DEV $ORIGIN_DEV $META_DEV

ulimit -c unlimited
./ddsnap agent $AGENT_SOCK_NAME
./ddsnap server $SNAPSTORE_DEV $ORIGIN_DEV $META_DEV $AGENT_SOCK_NAME $SERVER_SOCK_NAME 