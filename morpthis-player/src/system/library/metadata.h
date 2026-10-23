#ifndef METADATA_H
#define METADATA_H

#include <stdbool.h>

typedef struct {
	char title[256];
	char artist[256];		// performer credited on this track
	char album_artist[256]; // artist the album as a whole is credited to (TPE2 / ALBUMARTIST)
	char album[256];
	char genre[128];
	int track_number; // 0 if unknown
	int disc_number;  // 0 if unknown, which is read as disc one; see below
	int year;		  // 0 if unknown
	bool has_tags;	  // true if any tag field was found

	// Marked as a compilation: COMPILATION (or ITUNESCOMPILATION) in Vorbis
	// and APE tags, TCMP or TXXX:COMPILATION in ID3v2, cpil in MP4. Each track
	// of such a record has its own performer, and that is the name to show.
	bool compilation;

	// The series a book belongs to and its place in it, for the audiobook
	// index: SERIES / SERIES-PART and MOVEMENTNAME / MOVEMENT in Vorbis and APE
	// tags, TXXX:SERIES / TXXX:SERIES-PART, MVNM and MVIN in ID3v2, the same
	// freeform names and the movement atoms in MP4. The part is kept as the
	// text the tag holds ("3", "2.5", "3/12").
	char series[128];
	char series_part[32];

	// ReplayGain, when the file carries it: the loudness the tagger measured,
	// as a correction in dB, and the sample peak that correction has to stay
	// clear of. Track and album are separate numbers on purpose -- normalising
	// a whole record by one figure is what keeps a quiet passage quiet.
	//
	// Written by every tagger as text ("-7.50 dB", "0.988525"), which is why
	// they arrive here as floats rather than as tag strings.
	bool has_track_gain;
	bool has_album_gain;
	float track_gain_db;
	float album_gain_db;
	float track_peak; // 0 when the file does not say
	float album_peak;
} song_metadata_t;

// Reads whatever tag metadata is available for the file (ID3v1/ID3v2 for MP3,
// Vorbis comments for FLAC/OGG, LIST/INFO chunk for WAV). Always fills every
// field of `out` (empty string / 0 if not found). Does not touch playback
// state or decode any audio samples.
//
// disc_number is 0 when the file carries no disc tag, which is what a
// single-disc release looks like. Whoever orders by it treats that as disc
// one: a record where only some of the files carry the tag must not split
// into two shelves.
void metadata_read(const char *filepath, song_metadata_t *out);

// The artist to show for the track. By default the album's artist when the
// tags have one, since that is the name the record is filed under -- except on
// a compilation, where the album is credited to "Various Artists" or the like
// and the track's own performer says more. With "Use track artist" on, the
// track's own artist tag, whole -- every value of a tag that holds several, as
// the details show it -- and the album's artist only when the track has none.
const char *metadata_shown_artist(const song_metadata_t *m);

// "Use track artist", kept as [music] track_artist. Off by default.
bool metadata_track_artist(void);
void metadata_set_track_artist(bool on);

// The lyrics the file carries, as text: a Vorbis or APE LYRICS or
// UNSYNCEDLYRICS field, an ID3v2 USLT frame, an SYLT frame turned into LRC, or
// an MP4 ©lyr atom. Timed text wins over plain when a file has both. malloc'd,
// NULL when there are none.
char *metadata_read_lyrics(const char *filepath);

// metadata_read(), and the description the file carries as plain text, for
// an audiobook's summary: a Vorbis or APE DESCRIPTION, SUMMARY or COMMENT,
// an ID3v2 COMM frame or TXXX:DESCRIPTION, an MP4 ldes, desc or comment atom.
// HTML is reduced to text and the length capped at 8 KB. malloc'd, NULL when
// the file has none.
char *metadata_read_with_description(const char *filepath, song_metadata_t *out);

// ---------------------------------------------------------------------------
// The parsed tags, kept on the card beside the artwork cache
//
// metadata_read() is not one file access but four or five: a .cue beside the
// track, the header that says what format it is, the tag block itself, and the
// sidecars that might go with it. Measured at twenty-five milliseconds, and
// paid on the interface thread between the press of "next" and the frame that
// shows the new track -- where the whole rest of the change costs thirty.
//
// None of it can change unless the file does, and the entry is keyed on the
// file's own size and modification time exactly as the artwork is, so it is no
// less trustworthy than the picture beside it. The scan fills it in as it parses
// every track, which is work it is doing anyway.
//
// The cache lives in the artwork's database (cover.c) because that is where the
// connection, the lock and the key function already are; it is a second table
// in the same file, not a second file.
// ---------------------------------------------------------------------------

// False on a miss, or when there is no cache at all (no card, or a build with
// it disabled). *out untouched then -- call metadata_read().
bool metadata_cache_load(const char *filepath, song_metadata_t *out);

// Adds or replaces the entry. A write, so not for the interface thread: the
// player hands its misses to the bookkeeping and lets the poll write them.
void metadata_cache_store(const char *filepath, const song_metadata_t *meta);

// Closes the batch a run of stores keeps open. Called at the end of a scan, so
// the last entries are not left in a transaction a power-off would roll back.
void metadata_cache_flush(void);

#endif // METADATA_H
