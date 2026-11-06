// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
// Host test: Android Auto Channel 12 MediaPlaybackStatusService decode, AAIdrive-style virtual queue & Now Playing
// formatting, and shared-memory snapshot / cover art persistence (src/aa_media_status.hpp, src/aa_media_status.cpp).
#include "aa_media_status.hpp"
#include "kombi_nav_test_util.hpp"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

using namespace nbt;
using namespace nbt::aamedia;
using namespace kntest;

static bool fileExists(const char* p) {
    struct stat st;
    return stat(p, &st) == 0;
}

int main() {
    // 1. cleanText (AAIdrive UnicodeCleaner behavior) & formatDuration
    {
        std::string raw = "  \t Don\xe2\x80\x99t   Stop \xe2\x80\x93 \xe2\x80\x9cMix\xe2\x80\x9d\xe2\x80\xa6 \n ";
        assert(cleanText(raw) == "Don't Stop - \"Mix\"...");
        // UTF-8 boundary truncation: "Stra\xc3\x9fe" (7 bytes) capped at 5 bytes must not split \xc3\x9f
        assert(cleanText("Stra\xc3\x9f" "e", 5) == "Stra");
        assert(cleanText("Stra\xc3\x9f" "e", 6) == "Stra\xc3\x9f");
        assert(formatDuration(0) == "00:00");
        assert(formatDuration(65) == "01:05");
        assert(formatDuration(3661) == "1:01:01");
    }

    // 2. 0x8001 MediaPlaybackStatus decode
    {
        Bytes pb;
        fInt(pb, 1, kStatePlaying);
        fStr(pb, 2, "Spotify");
        fInt(pb, 3, 42);
        fInt(pb, 4, 1);
        fInt(pb, 5, kRepeatAll);

        MediaStatusMsg st;
        assert(parseMediaStatus(&pb[0], pb.size(), st));
        assert(st.has_state && st.state == kStatePlaying);
        assert(st.has_app && st.app == "Spotify");
        assert(st.has_pos && st.pos_s == 42u);
        assert(st.has_shuffle && st.shuffle);
        assert(st.has_repeat && st.repeat == kRepeatAll);

        // Truncated varint must fail cleanly
        Bytes bad;
        bad.push_back(0x08);
        assert(!parseMediaStatus(&bad[0], bad.size(), st));
    }

    // 3. 0x8003 MediaPlaybackMetadata decode (with cover art)
    {
        Bytes pb;
        fStr(pb, 1, "Midnight City");
        fStr(pb, 2, "M83");
        fStr(pb, 3, "Hurry Up, We\xe2\x80\x99re Dreaming");
        Bytes art;
        const uint8_t png_hdr[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        art.assign(png_hdr, png_hdr + 8);
        fBytes(pb, 4, art);
        fStr(pb, 5, "Synthwave Classics");
        fInt(pb, 6, 243);
        fInt(pb, 7, 1);

        MediaMetadataMsg md;
        assert(parseMediaMetadata(&pb[0], pb.size(), md));
        assert(md.has_title && md.title == "Midnight City");
        assert(md.has_artist && md.artist == "M83");
        assert(md.has_album && md.album == "Hurry Up, We're Dreaming");
        assert(md.has_art && md.album_art.size() == 8 && memcmp(&md.album_art[0], png_hdr, 8) == 0);
        assert(md.has_playlist && md.playlist == "Synthwave Classics");
        assert(md.has_dur && md.dur_s == 243u);
        assert(md.rating == 1);
    }

    // 4. AAIdrive-style 3-row virtual queue and media keycode / ctl mapping
    {
        MediaSnapshot snap;
        snap.state = kStatePlaying;
        snap.app = "Spotify";
        snap.title = "Starboy";
        snap.artist = "The Weeknd";
        snap.album = "Starboy";
        snap.pos_s = 75;
        snap.dur_s = 230;
        snap.shuffle = true;
        snap.repeat = kRepeatOne;

        std::vector<VirtualQueueRow> q;
        buildVirtualQueue(snap, q);
        assert(q.size() == 3);
        assert(q[0].action == kQueuePrev && q[0].label == "< Previous" && !q[0].selected);
        assert(q[1].action == kQueuePlayPause && q[1].label == "[Playing] Starboy - The Weeknd" && q[1].selected);
        assert(q[2].action == kQueueNext && q[2].label == "Next >" && !q[2].selected);

        assert(queueRowAction(0) == kQueuePrev);
        assert(queueRowAction(1) == kQueuePlayPause);
        assert(queueRowAction(2) == kQueueNext);
        assert(queueRowAction(3) == kQueueNone);

        assert(!strcmp(queueActionCtlCmd(kQueuePrev), "media prev"));
        assert(!strcmp(queueActionCtlCmd(kQueuePlayPause), "media playpause"));
        assert(!strcmp(queueActionCtlCmd(kQueueNext), "media next"));

        assert(queueActionKeycode(kQueuePrev) == AAK_MEDIA_PREVIOUS);
        assert(queueActionKeycode(kQueuePlayPause) == AAK_MEDIA_PLAY_PAUSE);
        assert(queueActionKeycode(kQueueNext) == AAK_MEDIA_NEXT);

        std::string np = formatNowPlayingText(snap, true);
        assert(np.find("Direct PCM (Browser 0xB8)") != std::string::npos);
        assert(np.find("App: Spotify") != std::string::npos);
        assert(np.find("Status: Playing  [01:15 / 03:50]") != std::string::npos);
        assert(np.find("Progress: [###-------]") != std::string::npos);
        assert(np.find("Track: Starboy") != std::string::npos);
        assert(np.find("Artist: The Weeknd") != std::string::npos);
        assert(np.find("Album: Starboy") != std::string::npos);
        assert(np.find("Shuffle=On") != std::string::npos);
        assert(np.find("Repeat=One") != std::string::npos);
    }

    // 5. AaMediaBridge end-to-end shared-memory snapshot + cover art lifecycle
    {
        char media_path[128], cover_path[128];
        snprintf(media_path, sizeof(media_path), "/tmp/nbt-aad-media-test-%d", (int)getpid());
        snprintf(cover_path, sizeof(cover_path), "/tmp/nbt-aad-cover-test-%d.img", (int)getpid());
        unlink(media_path);
        unlink(cover_path);

        AaMediaBridge::kMediaFile = media_path;
        AaMediaBridge::kCoverFile = cover_path;

        AaMediaBridge bridge;

        Bytes st_pb;
        fInt(st_pb, 1, kStatePlaying);
        fStr(st_pb, 2, "YouTube Music");
        fInt(st_pb, 3, 18);
        AaMediaBridge::mediaStatusCallback(0x8001, &st_pb[0], st_pb.size(), &bridge);

        Bytes md_pb, art;
        for (int i = 0; i < 64; ++i) art.push_back((uint8_t)(i & 0xFF));
        fStr(md_pb, 1, "Nightcall");
        fStr(md_pb, 2, "Kavinsky");
        fStr(md_pb, 3, "OutRun");
        fBytes(md_pb, 4, art);
        fInt(md_pb, 6, 258);
        AaMediaBridge::mediaStatusCallback(0x8003, &md_pb[0], md_pb.size(), &bridge);

        assert(fileExists(media_path));
        assert(fileExists(cover_path));

        MediaSnapshot read_back;
        assert(AaMediaBridge::readSnapshotFile(media_path, read_back));
        assert(read_back.seq == 2u);
        assert(read_back.pid == (int)getpid());
        assert(read_back.state == kStatePlaying);
        assert(read_back.pos_s == 18u);
        assert(read_back.dur_s == 258u);
        assert(read_back.art_seq == 1u);
        assert(read_back.art_len == 64u);
        assert(read_back.app == "YouTube Music");
        assert(read_back.title == "Nightcall");
        assert(read_back.artist == "Kavinsky");
        assert(read_back.album == "OutRun");

        // Next track without embedded art clears stale cover file
        Bytes md2_pb;
        fStr(md2_pb, 1, "Pacific Coast Highway");
        fStr(md2_pb, 2, "Kavinsky");
        fInt(md2_pb, 6, 378);
        AaMediaBridge::mediaStatusCallback(0x8003, &md2_pb[0], md2_pb.size(), &bridge);
        assert(!fileExists(cover_path));
        assert(AaMediaBridge::readSnapshotFile(media_path, read_back));
        assert(read_back.seq == 3u);
        assert(read_back.title == "Pacific Coast Highway");
        assert(read_back.art_len == 0u);

        // Session end cleans up both files
        bridge.onSessionEnded();
        assert(!fileExists(media_path));
        assert(!fileExists(cover_path));
    }

    printf("test_media_status OK\n");
    return 0;
}
