// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
// aa_media_status: Android Auto MediaPlaybackStatusService (ch 12) decoder, AAIdrive-style virtual queue / Now Playing
// formatter, and shared-memory publisher (/dev/shmem/nbt-aad-media + /dev/shmem/nbt-aad-cover.img) for libNbtAad.so.
//
// Channel 12 messages (aap_protobuf/service/mediaplayback):
//   0x8001 MediaPlaybackStatus   {1 state: 1 STOPPED, 2 PLAYING, 3 PAUSED; 2 media_source; 3 playback_seconds;
//                                 4 shuffle; 5 repeat: 0 OFF, 1 ALL, 2 ONE}
//   0x8003 MediaPlaybackMetadata {1 song; 2 artist; 3 album; 4 album_art (bytes); 5 playlist; 6 duration_seconds;
//                                 7 rating}
#ifndef AA_MEDIA_STATUS_HPP
#define AA_MEDIA_STATUS_HPP

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <pthread.h>
#include <sys/types.h>

#include "kombi_nav_msg.hpp"
#include "aa_keys.hpp"

namespace nbt {
namespace aamedia {

enum PlaybackState {
    kStateNone    = 0,
    kStateStopped = 1,
    kStatePlaying = 2,
    kStatePaused  = 3
};

enum RepeatMode {
    kRepeatOff = 0,
    kRepeatAll = 1,
    kRepeatOne = 2
};

// AAIdrive-style virtual queue actions (GlobalMetadata.kt / PlaybackView.kt: < Previous, current track, Next >).
enum QueueAction {
    kQueueNone      = 0,
    kQueuePrev      = 1,
    kQueuePlayPause = 2,
    kQueueNext      = 3
};

enum {
    kMaxTextBytes  = 120,
    kMaxCoverBytes = 262144
};

inline const char* stateName(int state) {
    switch (state) {
        case kStateStopped: return "Stopped";
        case kStatePlaying: return "Playing";
        case kStatePaused:  return "Paused";
        default:            return "Idle";
    }
}

// Sanitizes a UTF-8 string for iDrive RHMI display and single-line shared memory (modeled after AAIdrive's
// UnicodeCleaner.clean): strips control chars, normalizes common curly quotes / dashes / ellipsis to ASCII, trims
// outer spaces, and caps length on a UTF-8 codepoint boundary.
inline std::string cleanText(const uint8_t* data, size_t len, size_t max_bytes = kMaxTextBytes) {
    if (!data || len == 0 || max_bytes == 0) return std::string();
    std::string out;
    out.reserve(len < max_bytes ? len : max_bytes);
    size_t i = 0;
    bool last_space = true;   // trim leading spaces
    while (i < len && out.size() < max_bytes) {
        uint8_t c = data[i];
        if (c < 0x20 || c == 0x7F) {
            if (!last_space && out.size() + 1 <= max_bytes) {
                out.push_back(' ');
                last_space = true;
            }
            ++i;
            continue;
        }
        if (c < 0x80) {
            if (c == ' ') {
                if (!last_space) { out.push_back(' '); last_space = true; }
            } else {
                out.push_back((char)c);
                last_space = false;
            }
            ++i;
            continue;
        }
        // Normalize common 3-byte UTF-8 punctuation (U+2013..U+2026: E2 80 xx).
        if (c == 0xE2 && i + 2 < len && data[i + 1] == 0x80) {
            uint8_t b2 = data[i + 2];
            const char* repl = NULL;
            if (b2 == 0x98 || b2 == 0x99) repl = "'";        // curly single quotes
            else if (b2 == 0x9C || b2 == 0x9D) repl = "\"";  // curly double quotes
            else if (b2 == 0x93 || b2 == 0x94) repl = "-";   // en-dash / em-dash
            else if (b2 == 0xA6) repl = "...";               // horizontal ellipsis
            if (repl) {
                size_t rl = strlen(repl);
                if (out.size() + rl <= max_bytes) {
                    out.append(repl, rl);
                    last_space = false;
                }
                i += 3;
                continue;
            }
        }
        size_t k = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!k || i + k > len) {
            ++i;
            continue;
        }
        bool valid = true;
        for (size_t j = 1; j < k; ++j) {
            if ((data[i + j] & 0xC0) != 0x80) { valid = false; break; }
        }
        if (!valid) {
            ++i;
            continue;
        }
        if (out.size() + k > max_bytes) break;
        out.append((const char*)(data + i), k);
        last_space = false;
        i += k;
    }
    while (!out.empty() && out[out.size() - 1] == ' ') out.erase(out.size() - 1);
    return out;
}

inline std::string cleanText(const std::string& s, size_t max_bytes = kMaxTextBytes) {
    return s.empty() ? std::string() : cleanText((const uint8_t*)s.data(), s.size(), max_bytes);
}

// Formats seconds as "mm:ss" or "h:mm:ss" (like AAIdrive's TimeUtils.formatTime).
inline std::string formatDuration(uint32_t secs) {
    char buf[32];
    uint32_t h = secs / 3600u;
    uint32_t m = (secs % 3600u) / 60u;
    uint32_t s = secs % 60u;
    if (h > 0) snprintf(buf, sizeof(buf), "%u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
    else snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)m, (unsigned)s);
    return std::string(buf);
}

struct MediaStatusMsg {
    int state;              // PlaybackState (0..3)
    std::string app;        // field 2: source app name (e.g. "Spotify")
    uint32_t pos_s;         // field 3: elapsed seconds
    bool shuffle;           // field 4
    int repeat;             // field 5: RepeatMode (0..2)
    bool has_state;
    bool has_app;
    bool has_pos;
    bool has_shuffle;
    bool has_repeat;

    MediaStatusMsg()
        : state(kStateNone), pos_s(0), shuffle(false), repeat(kRepeatOff),
          has_state(false), has_app(false), has_pos(false), has_shuffle(false), has_repeat(false) {}
};

struct MediaMetadataMsg {
    std::string title;              // field 1: song
    std::string artist;             // field 2: artist
    std::string album;              // field 3: album
    std::vector<uint8_t> album_art; // field 4: raw PNG/JPEG cover art
    std::string playlist;           // field 5: playlist
    uint32_t dur_s;                 // field 6: duration_seconds
    int rating;                     // field 7: rating
    bool has_title;
    bool has_artist;
    bool has_album;
    bool has_art;
    bool has_playlist;
    bool has_dur;

    MediaMetadataMsg()
        : dur_s(0), rating(0),
          has_title(false), has_artist(false), has_album(false),
          has_art(false), has_playlist(false), has_dur(false) {}
};

// Decodes 0x8001 MediaPlaybackStatus payload (bytes after the 2-byte msg_id).
inline bool parseMediaStatus(const uint8_t* body, size_t len, MediaStatusMsg& out) {
    out = MediaStatusMsg();
    if (!body && len > 0) return false;
    kombinav::PbReader r(body, len);
    kombinav::PbField f;
    while (r.next(f)) {
        if (f.num == 1 && f.wire == 0) {
            out.state = (int)f.value;
            out.has_state = true;
        } else if (f.num == 2 && f.wire == 2) {
            out.app = cleanText(f.data, f.len);
            out.has_app = true;
        } else if (f.num == 3 && f.wire == 0) {
            out.pos_s = (uint32_t)f.value;
            out.has_pos = true;
        } else if (f.num == 4 && f.wire == 0) {
            out.shuffle = (f.value != 0);
            out.has_shuffle = true;
        } else if (f.num == 5 && f.wire == 0) {
            out.repeat = (int)f.value;
            out.has_repeat = true;
        }
    }
    return r.ok();
}

// Decodes 0x8003 MediaPlaybackMetadata payload (bytes after the 2-byte msg_id).
inline bool parseMediaMetadata(const uint8_t* body, size_t len, MediaMetadataMsg& out) {
    out = MediaMetadataMsg();
    if (!body && len > 0) return false;
    kombinav::PbReader r(body, len);
    kombinav::PbField f;
    while (r.next(f)) {
        if (f.num == 1 && f.wire == 2) {
            out.title = cleanText(f.data, f.len);
            out.has_title = true;
        } else if (f.num == 2 && f.wire == 2) {
            out.artist = cleanText(f.data, f.len);
            out.has_artist = true;
        } else if (f.num == 3 && f.wire == 2) {
            out.album = cleanText(f.data, f.len);
            out.has_album = true;
        } else if (f.num == 4 && f.wire == 2) {
            out.has_art = true;
            if (f.len > 0 && f.len <= (size_t)kMaxCoverBytes) {
                out.album_art.assign(f.data, f.data + f.len);
            }
        } else if (f.num == 5 && f.wire == 2) {
            out.playlist = cleanText(f.data, f.len);
            out.has_playlist = true;
        } else if (f.num == 6 && f.wire == 0) {
            out.dur_s = (uint32_t)f.value;
            out.has_dur = true;
        } else if (f.num == 7 && f.wire == 0) {
            out.rating = (int)f.value;
        }
    }
    return r.ok();
}

// Combined snapshot of the current Android Auto media session, shared with libNbtAad.so via /dev/shmem/nbt-aad-media.
struct MediaSnapshot {
    uint32_t seq;           // bumped on any metadata or playback state/position update
    int state;              // PlaybackState (0..3)
    uint32_t pos_s;         // elapsed seconds
    uint32_t dur_s;         // total duration seconds
    bool shuffle;
    int repeat;
    uint32_t art_seq;       // bumped when cover art bytes change
    uint32_t art_len;       // size of /dev/shmem/nbt-aad-cover.img (0 = none)
    int pid;                // nbt-aad pid that wrote this record
    std::string app;
    std::string title;
    std::string artist;
    std::string album;

    MediaSnapshot()
        : seq(0), state(kStateNone), pos_s(0), dur_s(0), shuffle(false), repeat(kRepeatOff),
          art_seq(0), art_len(0), pid(0) {}

    bool hasTrack() const {
        return !title.empty() || !artist.empty() || !album.empty() || state == kStatePlaying || state == kStatePaused;
    }
};

// AAIdrive-style 3-row virtual queue item (GlobalMetadata.prepareQueue / PlaybackView.redrawAudiostatePlaylist).
struct VirtualQueueRow {
    QueueAction action;
    std::string label;
    bool selected;
    VirtualQueueRow() : action(kQueueNone), selected(false) {}
    VirtualQueueRow(QueueAction a, const std::string& l, bool sel) : action(a), label(l), selected(sel) {}
};

inline void buildVirtualQueue(const MediaSnapshot& snap, std::vector<VirtualQueueRow>& out) {
    out.clear();
    out.reserve(3);
    out.push_back(VirtualQueueRow(kQueuePrev, "< Previous", false));
    std::string mid;
    if (!snap.title.empty() && !snap.artist.empty()) {
        mid = snap.title + " - " + snap.artist;
    } else if (!snap.title.empty()) {
        mid = snap.title;
    } else if (!snap.app.empty()) {
        mid = snap.app;
    } else {
        mid = "Now Playing";
    }
    if (snap.state == kStatePlaying) mid = "[Playing] " + mid;
    else if (snap.state == kStatePaused) mid = "[Paused] " + mid;
    out.push_back(VirtualQueueRow(kQueuePlayPause, mid, true));
    out.push_back(VirtualQueueRow(kQueueNext, "Next >", false));
}

inline QueueAction queueRowAction(int index) {
    if (index == 0) return kQueuePrev;
    if (index == 1) return kQueuePlayPause;
    if (index == 2) return kQueueNext;
    return kQueueNone;
}

inline const char* queueActionCtlCmd(QueueAction a) {
    switch (a) {
        case kQueuePrev:      return "media prev";
        case kQueuePlayPause: return "media playpause";
        case kQueueNext:      return "media next";
        default:              return "";
    }
}

inline uint32_t queueActionKeycode(QueueAction a) {
    switch (a) {
        case kQueuePrev:      return AAK_MEDIA_PREVIOUS;
        case kQueuePlayPause: return AAK_MEDIA_PLAY_PAUSE;
        case kQueueNext:      return AAK_MEDIA_NEXT;
        default:              return 0;
    }
}

// Formats the multi-line Now Playing card for the iDrive Richtext view (hmiState 14), following AAIdrive's
// PlaybackView layout (App header, Track, Artist, Album, Playback State + Progress bar / Time).
inline std::string formatNowPlayingText(const MediaSnapshot& snap, bool direct_audio = true) {
    std::string out;
    out.reserve(384);
    out += "Audio route: ";
    out += direct_audio ? "Direct PCM (Browser 0xB8)" : "Bluetooth A2DP";
    out += "\n";

    if (!snap.hasTrack() && snap.app.empty()) {
        out += "Status: Idle (no active media stream)\n";
        out += "\nStart music on your phone or use Play / Pause, Next track, and Previous track in the Android Auto menu.";
        return out;
    }

    if (!snap.app.empty()) {
        out += "App: ";
        out += snap.app;
        out += "\n";
    }
    out += "Status: ";
    out += stateName(snap.state);
    if (snap.dur_s > 0 || snap.pos_s > 0) {
        out += "  [";
        out += formatDuration(snap.pos_s);
        if (snap.dur_s > 0) {
            out += " / ";
            out += formatDuration(snap.dur_s);
        }
        out += "]";
    }
    out += "\n";

    if (snap.dur_s > 0) {
        uint32_t pct = snap.pos_s >= snap.dur_s ? 10u : (uint32_t)((snap.pos_s * 10u) / snap.dur_s);
        out += "Progress: [";
        for (uint32_t i = 0; i < 10u; ++i) out.push_back(i < pct ? '#' : '-');
        out += "]\n";
    }

    out += "Track: ";
    out += snap.title.empty() ? "-" : snap.title;
    out += "\nArtist: ";
    out += snap.artist.empty() ? "-" : snap.artist;
    out += "\nAlbum: ";
    out += snap.album.empty() ? "-" : snap.album;

    if (snap.shuffle || snap.repeat != kRepeatOff) {
        out += "\nMode:";
        if (snap.shuffle) out += " Shuffle=On";
        if (snap.repeat == kRepeatAll) out += " Repeat=All";
        else if (snap.repeat == kRepeatOne) out += " Repeat=One";
    }
    return out;
}

// Daemon-side runtime tracker and shared-memory writer. Thread-safe.
class AaMediaBridge {
public:
    AaMediaBridge();
    ~AaMediaBridge();

    // AASession media-status callback (session thread).
    static void mediaStatusCallback(uint16_t msg_id, const uint8_t* body, size_t len, void* user);
    void onMessage(uint16_t msg_id, const uint8_t* body, size_t len);
    void onSessionEnded();
    void stop();

    MediaSnapshot snapshot() const;

    // Shared-memory file paths (variables so host tests can override them).
    static const char* kMediaFile;   // /dev/shmem/nbt-aad-media
    static const char* kCoverFile;   // /dev/shmem/nbt-aad-cover.img

    // Shared-memory serialization / deserialization helpers (used by both daemon and plugin/tests).
    static bool writeSnapshotFile(const char* path, const MediaSnapshot& snap);
    static bool writeCoverFile(const char* path, const uint8_t* data, size_t len);

    static inline bool readSnapshotFile(const char* path, MediaSnapshot& out) {
        out = MediaSnapshot();
        if (!path || !*path) return false;
        FILE* f = fopen(path, "r");
        if (!f) return false;
        char line[256];
        bool any = false;
        while (fgets(line, sizeof(line), f)) {
            size_t l = strlen(line);
            while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
            if (strncmp(line, "seq ", 4) == 0) {
                out.seq = (uint32_t)strtoul(line + 4, NULL, 10);
                any = true;
            } else if (strncmp(line, "pid ", 4) == 0) {
                out.pid = atoi(line + 4);
            } else if (strncmp(line, "state ", 6) == 0) {
                out.state = atoi(line + 6);
            } else if (strncmp(line, "pos ", 4) == 0) {
                out.pos_s = (uint32_t)strtoul(line + 4, NULL, 10);
            } else if (strncmp(line, "dur ", 4) == 0) {
                out.dur_s = (uint32_t)strtoul(line + 4, NULL, 10);
            } else if (strncmp(line, "shuffle ", 8) == 0) {
                out.shuffle = atoi(line + 8) != 0;
            } else if (strncmp(line, "repeat ", 7) == 0) {
                out.repeat = atoi(line + 7);
            } else if (strncmp(line, "art_seq ", 8) == 0) {
                out.art_seq = (uint32_t)strtoul(line + 8, NULL, 10);
            } else if (strncmp(line, "art_len ", 8) == 0) {
                out.art_len = (uint32_t)strtoul(line + 8, NULL, 10);
            } else if (strncmp(line, "app ", 4) == 0) {
                out.app = line + 4;
            } else if (strncmp(line, "title ", 6) == 0) {
                out.title = line + 6;
            } else if (strncmp(line, "artist ", 7) == 0) {
                out.artist = line + 7;
            } else if (strncmp(line, "album ", 6) == 0) {
                out.album = line + 6;
            }
        }
        fclose(f);
        return any;
    }

private:
    mutable pthread_mutex_t mu_;
    MediaSnapshot snap_;
};

} // namespace aamedia
} // namespace nbt

#endif // AA_MEDIA_STATUS_HPP
