#!/bin/sh
# Morpthis: start the AirPlay receiver.
#
#   shairport_on.sh <name>
#
# sys_server pastes the argument into a shell command unquoted, so the player
# sends the advertised name with spaces turned into '%'. Put them back.
name="$1"
[ -n "$name" ] || name="Morpthis"
name=$(printf '%s' "$name" | sed 's/%/ /g')

# The maintained daemon. It writes raw S16_LE 44100 stereo into a pipe the
# player reads, and metadata into a second one; see /etc/shairport-sync.conf.
# Its output is detached from the daemon's stdout so sys_server's system()
# call can return: the daemon must outlive the script.
if [ -x /usr/bin/shairport-sync ]; then
	killall shairport-sync 2>/dev/null
	/usr/bin/shairport-sync -c /etc/shairport-sync.conf -a "$name" >/dev/null 2>&1 &
	exit 0
fi

# Fallback for an image built before shairport-sync: the stock daemon and its
# private "ot" socket backend, which the player still knows how to read.
killall shairport 2>/dev/null
/usr/bin/shairport -a "$name" -o ot -M /tmp -b 160 >/dev/null 2>&1 &
