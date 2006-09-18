#!/bin/bash 

cd /src/2.6.17-rc6-dd/drivers/md/ddraid/scripts

# import all the sweet variables used throughout the ddsnap scripts
. ddsnap_env.sh

# check to see if replication is in progress
command_name=`basename $0`
out=`ps -ef | grep $command_name | grep -v grep | grep -v $$ | grep -iv cron`
if [ -n "$out" ] 
then
	echo "replication is already in process. exiting." 
	exit 0
fi

# now let's see the next snapshot to create
num_snapshots=`/sbin/dmsetup ls | grep $SNAPSHOT_VOL_NAME | wc -l`
new_snapshot=$num_snapshots

if [[ $num_snapshots -ge $MAX_SNAPSHOTS ]]
then
	# FIXME: check for return code for each command
	# need to delete a snapshot... oldest one for now?
	oldest_snapshot=`./${LIST_SNAPSHOT} | grep ctime | awk '{printf "%s %s %s %s %s\n", $7, $8, $9, $10, $2} ' | sort | head -n 1 | awk '{print $5}'`
	./dd_delete_snap.sh $oldest_snapshot
	./dd_delete_rsnap.sh $oldest_snapshot
	new_snapshot=$oldest_snapshot
fi
	# check to make sure there are no holes within the number of snapshots
	# FIXME: just assume no one will be removing snapshots... only this script will do it	

# now we have a new_snapshot to create and then replicate it 
echo "creating snapshot" 
./dd_create_snap.sh $new_snapshot
#previous version 
prev_snapshot=$(( $new_snapshot - 1))
if [[ $prev_snapshot -eq -1 ]]
then
	prev_snapshot=$(( $MAX_SNAPSHOTS - 1))
fi

echo "syncing mount drive" 
cd /homevol
sync
cd -

# replicate
# FIXME: need to check if we can create the damn files... no space left problems will exist :)
echo "replicating: dd_replicate.sh -x $prev_snapshot $new_snapshot"  
./dd_replicate.sh -r $prev_snapshot $new_snapshot