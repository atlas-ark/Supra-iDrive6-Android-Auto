// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
#include "aa_media_status.hpp"

#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

namespace nbt {
namespace aamedia {

const char* AaMediaBridge::kMediaFile = "/dev/shmem/nbt-aad-media";
const char* AaMediaBridge::kCoverFile = "/dev/shmem/nbt-aad-cover.img";

AaMediaBridge::AaMediaBridge() {
    pthread_mutex_init(&mu_, NULL);
}

AaMediaBridge::~AaMediaBridge() {
    pthread_mutex_destroy(&mu_);
}

void AaMediaBridge::mediaStatusCallback(uint16_t msg_id, const uint8_t* body, size_t len, void* user) {
    if (!user) return;
    static_cast<AaMediaBridge*>(user)->onMessage(msg_id, body, len);
}

bool AaMediaBridge::writeCoverFile(const char* path, const uint8_t* data, size_t len) {
    if (!path || !*path) return false;
    if (!data || len == 0 || len > (size_t)kMaxCoverBytes) {
        unlink(path);
        return false;
    }
    int fd = open(path, O_WRONLY | O_CREAT, 0666);
    if (fd < 0) return false;
    fchmod(fd, 0666);
    if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
        close(fd);
        return false;
    }
    size_t total = 0;
    while (total < len) {
        ssize_t w = write(fd, data + total, len - total);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            close(fd);
            return false;
        }
        total += (size_t)w;
    }
    if (ftruncate(fd, (off_t)len) != 0) {}
    close(fd);
    return true;
}

bool AaMediaBridge::writeSnapshotFile(const char* path, const MediaSnapshot& snap) {
    if (!path || !*path) return false;
    char buf[1024];
    int pid = snap.pid > 0 ? snap.pid : (int)getpid();
    int n = snprintf(buf, sizeof(buf),
                     "seq %u\n"
                     "pid %d\n"
                     "state %d\n"
                     "pos %u\n"
                     "dur %u\n"
                     "shuffle %d\n"
                     "repeat %d\n"
                     "art_seq %u\n"
                     "art_len %u\n"
                     "app %s\n"
                     "title %s\n"
                     "artist %s\n"
                     "album %s\n",
                     (unsigned)snap.seq,
                     pid,
                     snap.state,
                     (unsigned)snap.pos_s,
                     (unsigned)snap.dur_s,
                     snap.shuffle ? 1 : 0,
                     snap.repeat,
                     (unsigned)snap.art_seq,
                     (unsigned)snap.art_len,
                     snap.app.c_str(),
                     snap.title.c_str(),
                     snap.artist.c_str(),
                     snap.album.c_str());
    if (n <= 0 || n >= (int)sizeof(buf)) return false;
    int fd = open(path, O_WRONLY | O_CREAT, 0666);
    if (fd < 0) return false;
    fchmod(fd, 0666);
    if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
        close(fd);
        return false;
    }
    ssize_t w = write(fd, buf, (size_t)n);
    if (w == (ssize_t)n && ftruncate(fd, (off_t)n) != 0) {}
    close(fd);
    return w == (ssize_t)n;
}

void AaMediaBridge::onMessage(uint16_t msg_id, const uint8_t* body, size_t len) {
    if (msg_id == 0x8001) {
        MediaStatusMsg st;
        if (!parseMediaStatus(body, len, st)) return;
        MediaSnapshot copy;
        bool log_change = false;
        pthread_mutex_lock(&mu_);
        int prev_state = snap_.state;
        std::string prev_app = snap_.app;
        if (st.has_state) snap_.state = st.state;
        if (st.has_app && !st.app.empty()) snap_.app = st.app;
        if (st.has_pos) snap_.pos_s = st.pos_s;
        if (st.has_shuffle) snap_.shuffle = st.shuffle;
        if (st.has_repeat) snap_.repeat = st.repeat;
        snap_.pid = (int)getpid();
        ++snap_.seq;
        copy = snap_;
        log_change = (snap_.state != prev_state) || (snap_.app != prev_app);
        pthread_mutex_unlock(&mu_);

        if (log_change) {
            printf("[Media] status ch12: state=%s (%d) app='%s' pos=%us\n",
                   stateName(copy.state), copy.state, copy.app.c_str(), (unsigned)copy.pos_s);
        }
        writeSnapshotFile(kMediaFile, copy);
    } else if (msg_id == 0x8003) {
        MediaMetadataMsg md;
        if (!parseMediaMetadata(body, len, md)) return;
        MediaSnapshot copy;
        bool log_track = false;
        pthread_mutex_lock(&mu_);
        std::string prev_title = snap_.title;
        std::string prev_artist = snap_.artist;
        if (md.has_title) snap_.title = md.title;
        if (md.has_artist) snap_.artist = md.artist;
        if (md.has_album) snap_.album = md.album;
        if (md.has_dur) snap_.dur_s = md.dur_s;
        if (md.has_art && !md.album_art.empty()) {
            if (writeCoverFile(kCoverFile, &md.album_art[0], md.album_art.size())) {
                ++snap_.art_seq;
                snap_.art_len = (uint32_t)md.album_art.size();
            }
        } else if (md.has_title && prev_title != snap_.title) {
            // New track without embedded cover art: clear stale cover art.
            unlink(kCoverFile);
            snap_.art_len = 0;
        }
        snap_.pid = (int)getpid();
        ++snap_.seq;
        copy = snap_;
        log_track = (snap_.title != prev_title) || (snap_.artist != prev_artist);
        pthread_mutex_unlock(&mu_);

        if (log_track) {
            printf("[Media] metadata ch12: '%s' - '%s' (album '%s', dur %us, cover %u B)\n",
                   copy.title.c_str(), copy.artist.c_str(), copy.album.c_str(),
                   (unsigned)copy.dur_s, (unsigned)copy.art_len);
        }
        writeSnapshotFile(kMediaFile, copy);
    }
}

void AaMediaBridge::onSessionEnded() {
    pthread_mutex_lock(&mu_);
    snap_ = MediaSnapshot();
    pthread_mutex_unlock(&mu_);
    if (kMediaFile && *kMediaFile) unlink(kMediaFile);
    if (kCoverFile && *kCoverFile) unlink(kCoverFile);
}

void AaMediaBridge::stop() {
    onSessionEnded();
}

MediaSnapshot AaMediaBridge::snapshot() const {
    pthread_mutex_lock(&mu_);
    MediaSnapshot copy = snap_;
    pthread_mutex_unlock(&mu_);
    return copy;
}

} // namespace aamedia
} // namespace nbt
