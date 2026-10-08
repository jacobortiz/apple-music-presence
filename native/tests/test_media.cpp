#include "amp/media.hpp"

#include <cassert>
#include <limits>

void test_media() {
    using namespace amp;
    using namespace amp::media_detail;

    assert(is_apple_music_source("AppleInc.AppleMusicWin_nzyj5cx40ttqa!App"));
    assert(is_apple_music_source("C:\\Program Files\\AppleMusic.exe"));
    assert(is_apple_music_source("APPLEMusic.exe"));
    assert(!is_apple_music_source("Spotify.exe"));
    assert(!is_apple_music_source("AppleMusic.exe.bak"));
    assert(!is_apple_music_source("Chrome.exe"));

    Track combined{"Song", "Artist \xE2\x80\x94 Album", ""};
    normalize_artist_album(combined, "AppleMusic.exe");
    assert(combined.artist == "Artist" && combined.album == "Album");
    Track explicit_album{"Song", "Artist \xE2\x80\x94 Other", "Album"};
    normalize_artist_album(explicit_album, "AppleMusic.exe");
    assert(explicit_album.artist == "Artist \xE2\x80\x94 Other");
    Track ambiguous{"Song", "Artist \xE2\x80\x94 Album \xE2\x80\x94 Live", ""};
    normalize_artist_album(ambiguous, "AppleMusic.exe");
    assert(ambiguous.album.empty());
    Track foreign{"Song", "Artist \xE2\x80\x94 Album", ""};
    normalize_artist_album(foreign, "Spotify.exe");
    assert(foreign.album.empty());

    Snapshot snapshot;
    snapshot.state = PlaybackState::playing;
    snapshot.observed_at = 110;
    apply_timeline(snapshot, 5, 205, 15, 100);
    assert(snapshot.duration == 200 && snapshot.position == 20);
    snapshot.playback_rate = 2;
    apply_timeline(snapshot, 5, 205, 15, 100);
    assert(snapshot.position == 30);
    snapshot.state = PlaybackState::paused;
    apply_timeline(snapshot, 5, 205, 15, 100);
    assert(snapshot.position == 10);
    snapshot.state = PlaybackState::playing;
    apply_timeline(snapshot, 5, 205, 15, 120);
    assert(snapshot.position == 10);  // Future timestamps must not rewind.
    apply_timeline(snapshot, 5, 205, 15, -11644473600.0);
    assert(snapshot.position == 10);  // WinRT's empty 1601 DateTime.
    apply_timeline(snapshot, 5, 205, 204, 100);
    assert(snapshot.position == 200);
    apply_timeline(snapshot, 5, 5, 3, 0);
    assert(!snapshot.duration && snapshot.position == 0);
    apply_timeline(snapshot, 0, 200, std::numeric_limits<double>::infinity(), 100);
    assert(!snapshot.position && snapshot.duration == 200);
    apply_timeline(snapshot, std::numeric_limits<double>::quiet_NaN(), 200, 15, 100);
    assert(!snapshot.position && !snapshot.duration);
}
