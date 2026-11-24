// SPDX-License-Identifier: GPL-3.0-or-later
//
// Network media through libVLC: servers found on the local network, folders on them, and saved
// servers and addresses.
//
//   discovery  libVLC's LAN services discoverers (UPnP/DLNA media servers; mDNS for SMB, NFS, FTP and
//              SFTP servers that advertise themselves), started with the browser, read when asked
//   listing    a location (smb://nas/films/, upnp://..., ftp://..., nfs://...) is parsed by libVLC
//              with network access and interaction allowed: its sub-items are the folder's entries.
//              Sign-in requests reach the app through libVLC's dialog API (vlc_dialogs.hpp).
//   saved      servers the viewer added by address, and recently opened stream addresses, in the
//              app's key/value store (JSON)
//
// Builds without libVLC (the host build) get a demo browser with made-up servers and folders, for
// the UI and its tests.
//
// Threading: servers() from any thread; list() blocks (a worker), cancellable.
#pragma once

#include "wnk/async/async.hpp"
#include "wnk/platform/files.hpp"
#include "wnk/storage/kv.hpp"

#include <memory>
#include <string>
#include <vector>

struct libvlc_instance_t;

namespace vplayer {

struct NetEntry {
    enum class Kind : uint8_t { Server, Folder, Video, Audio, Subtitle, Media, Other };
    std::string name;
    std::string mrl;     // what libVLC opens (and lists, for servers and folders)
    Kind kind = Kind::Other;
    std::string detail;  // "SMB share", "DLNA media server", ...
    std::vector<std::string> subtitles;  // subtitle files beside it, as VLC matched them (their MRLs)
    bool playable() const { return kind == Kind::Video || kind == Kind::Audio || kind == Kind::Media; }
};

class NetworkBrowser {
 public:
    virtual ~NetworkBrowser() = default;
    // Servers found so far (discovery keeps running while the browser lives).
    virtual std::vector<NetEntry> servers() = 0;
    // The entries of `mrl`, folders first then files, natural order. Blocking. False with a reason.
    virtual bool list(const std::string &mrl, std::vector<NetEntry> *out, std::string *error,
                      const wnk::async::CancellationToken &cancel) = 0;
};

// libVLC's browser when `vlc` is set, else the demo one.
std::unique_ptr<NetworkBrowser> make_network_browser(libvlc_instance_t *vlc);

// The entries of a network folder through libVLC, without starting discovery. Blocking. `interact`:
// VLC may ask (a sign-in); else a folder needing one fails.
// `show_all`: also the files VLC's listing leaves out (subtitles, pictures: its ignore-filetypes).
bool list_network_folder(libvlc_instance_t *vlc, const std::string &mrl, std::vector<NetEntry> *out, std::string *error,
                         const wnk::async::CancellationToken &cancel, bool interact = true, bool show_all = false);
// The subtitle files beside a network video: its folder listed (no questions asked), and the files
// VLC's listing matched to it (VLC 3 adds them to what plays only for local files). Call once the
// video plays (VLC then holds its sign-in). Blocking (a worker). Empty without libVLC.
std::vector<std::string> find_sidecar_subtitles(libvlc_instance_t *vlc, const std::string &video_mrl,
                                                const wnk::async::CancellationToken &cancel);

// ---- helpers (pure, tested on the host) ----

// "smb://nas/Films%20HD/" → "Films HD"; the host for a bare server ("smb://nas/" → "nas").
std::string mrl_display_name(const std::string &mrl);
// "smb://nas/films/a.mkv" → "smb://nas/films/"; "" at a server's root.
std::string mrl_parent(const std::string &mrl);
// The scheme in capitals for people: "SMB", "NFS", "FTP", "SFTP", "DLNA" (upnp), "HTTP"...
std::string mrl_protocol(const std::string &mrl);
// Whether people's input looks like something libVLC can open ("http://", "smb://", "rtsp://"...).
bool looks_like_url(const std::string &text);
// What an entry is from its name (extension) and whether libVLC called it a folder.
NetEntry::Kind net_kind(const std::string &name, bool folder);
// For the player: the entry as a FileEntry whose path is the MRL.
wnk::platform::FileEntry to_file_entry(const NetEntry &e);

// ---- saved servers and addresses ----

struct SavedPlaces {
    std::vector<NetEntry> servers;      // kind Server
    std::vector<std::string> urls;      // newest first, at most 10
};
SavedPlaces load_saved_places(wnk::storage::KeyValueStore *store);
bool save_saved_places(wnk::storage::KeyValueStore *store, const SavedPlaces &places);
std::string saved_places_json(const SavedPlaces &places);
SavedPlaces saved_places_from_json(const std::string &json);
void remember_url(SavedPlaces *places, const std::string &url);

}  // namespace vplayer
