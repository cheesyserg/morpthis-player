#ifndef AIRPLAY_H
#define AIRPLAY_H

#include <stdbool.h>

// AirPlay, with the player owning the DAC.
//
// Two receivers are supported:
//
//   shairport-sync   the maintained daemon, classic AirPlay only. Decodes ALAC
//                    to raw S16_LE 44100 stereo and writes it to a FIFO
//                    (/tmp/shairport-sync-audio). Its metadata pipe carries
//                    XML items the player parses for title/artist/album and
//                    session events. This is what an image carrying
//                    /usr/bin/shairport-sync uses by default.
//
//   shairport        the stock daemon, kept as a fallback. It has no ALSA
//                    backend at all: its `-o ot` output connects to a unix
//                    socket the player listens on
//                    (/var/run/airplay_socket_server), and its metadata uses a
//                    second socket with a private text protocol.
//
// In both cases the daemon never opens the DAC: the player reads the PCM and
// feeds it through audio_external_*(), so routing, volume, LED, standby and
// the hand-back to local playback stay in one place. The daemon is started and
// stopped through sys_server ('SHAIRPORT:TURN_ON:<name>' / 'TURN_OFF') and the
// scripts in /usr/bin, and it stays on until switched off -- a phone can keep
// playing while the player is used for something else.
//
// The name is read from /usr/resource/bt_name ("HiBy R3PROII") with spaces
// turned into '%', which the scripts turn back into spaces before passing it
// to the daemon. That is not decoration: sys_server builds its command line
// with sprintf and passes it to system(), so a name with a real space in it
// would arrive as two arguments.

#define AIRPLAY_TEXT_MAX 256

typedef struct {
	bool connected; // a sender has claimed the stream
	bool playing;	// ...and audio is arriving
	char title[AIRPLAY_TEXT_MAX];
	char artist[AIRPLAY_TEXT_MAX];
	char album[AIRPLAY_TEXT_MAX];
	char genre[AIRPLAY_TEXT_MAX];
} airplay_state_t;

// True when the firmware carries the selected receiver and its scripts.
bool airplay_available(void);

// Brings the selected receiver up and asks sys_server to start it. Returns
// immediately; the pipes (or sockets) and the request happen on a worker,
// because sys_server runs its scripts through a blocking system().
//
// Local playback is paused for you, when and only when a sender actually
// starts: switching the receiver on is not the same as using it, and there is
// no reason to stop the music for a phone that never connects.
void airplay_set_enabled(bool on);
bool airplay_get_enabled(void);

// True once the pipes (or sockets) are ready and the daemon has been asked for.
// There is a moment between the switch and this: sys_server queues the request
// and only looks at its queue once a second, and starting the servers comes
// first.
bool airplay_running(void);

// What the receiver is doing right now. Safe to call from the interface thread;
// it copies under a lock.
void airplay_get_state(airplay_state_t *out);

// Bumped whenever anything in the state changes, so a page can redraw only when
// there is something new rather than every tick.
unsigned airplay_serial(void);

// Hands the DAC back, so local playback -- a track on the card, an audiobook,
// an app -- can open it.
//
// It has to be asked for, because neither receiver can tell on its own that a
// sender has finished: the daemon keeps running (and, with the stock one, keeps
// its audio socket) for as long as the receiver is switched on. So "the stream
// is live" stays true for the whole time AirPlay is on, and without this the
// PCM opened for it is held -- silent, but held -- and a local track cannot
// open the device.
//
// Refused while audio is actually arriving: a sender in the middle of a song
// owns the output, which is the same rule that pauses a local track when a
// sender starts, read the other way round. True means the device is free, or
// about to be -- the hand-back happens on the AirPlay thread within a poll, and
// the PCM open that follows retries for two seconds anyway.
bool airplay_release_output(void);

// Milliseconds since the last audio arrived, or -1 if none ever has. The stock
// player uses this to close its AirPlay screen after thirty seconds without
// data, telling the user the screen will close; a sender that pauses stops
// sending entirely, so a long silence really does mean the session is over
// rather than quiet.
int airplay_silent_ms(void);

// The name senders see in their AirPlay list.
const char *airplay_name(void);

#endif /* AIRPLAY_H */
