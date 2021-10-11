#
# Regular cron jobs for the libneatvnc package
#
0 4	* * *	root	[ -x /usr/bin/libneatvnc_maintenance ] && /usr/bin/libneatvnc_maintenance
