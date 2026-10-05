#!/usr/bin/env python3
"""fakeplex.py PORT - a stand-in for plex.tv and a Plex Media Server, for
the host tests. It answers the requests Matinee makes with fixed JSON, and
records every request (method, path, query, headers, body); GET /_log
returns the record as JSON, POST /_reset clears it.

The part file /library/parts/11/111/file.mp4 is 5 MB of a fixed pattern
(Range is honoured, as Plex does)."""
import json, sys, threading, hashlib, gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs

PORT = int(sys.argv[1])
ACCOUNT = "ACCT-TOKEN"
SERVER = "SRV-TOKEN"
KID = "KID-ACCT"                # Plex Home: the account token of the user "Kids"
KID_SERVER = "KID-SRV"          # and the server's token for them
SERVERS = (SERVER, KID_SERVER)
ACCOUNTS = (ACCOUNT, KID)
HOME_USERS = [
    {"id": 1, "uuid": "u-admin", "title": "Andrew", "username": "andrew", "admin": True, "guest": False,
     "restricted": False, "protected": True, "thumb": "https://plex.tv/users/1/avatar"},
    {"id": 2, "uuid": "u-kids", "title": "Kids", "username": "", "admin": False, "guest": False,
     "restricted": True, "protected": False, "thumb": "https://plex.tv/users/2/avatar"},
]
RATED = {}                      # rating key -> the user's rating (PUT /:/rate)
LOG = []
LOCK = threading.Lock()
PIN_POLLS = {"n": 0}
PART = bytes((i * 7 + (i >> 11)) & 255 for i in range(5 * 1024 * 1024))
# a small valid baseline JPEG would do for the tests' purposes; the bytes
# only have to arrive intact
def jpeg(url):
    """A stand-in JPEG: the right first and last bytes, and the picture's
    address inside (so each poster is different)."""
    if "/library/metadata/103/thumb" in url:     # cut short on the way (no EOI): not to be cached
        return b"\xff\xd8\xff\xe0" + b"MATINEE-TEST-JPEG" + url.encode()
    return b"\xff\xd8\xff\xe0" + b"MATINEE-TEST-JPEG" + url.encode() + b"\xff\xd9"


ONDECK_GONE = set()          # rating keys taken off Continue watching
SUBSEL = {}                 # part id -> the subtitle stream chosen (PUT /library/parts)
AUDSEL = {}                 # part id -> the sound stream chosen


def streams(m):
    """A film's streams as the metadata of one item gives them: video,
    sound, and for Big Buck Bunny three subtitle tracks."""
    rk = int(m["ratingKey"])
    pid = 11000 + rk
    st = [{"id": rk * 10 + 1, "streamType": 1, "codec": m["Media"][0]["videoCodec"]},
          {"id": rk * 10 + 2, "streamType": 2, "codec": m["Media"][0]["audioCodec"],
           "displayTitle": "English (AAC Stereo)"}]
    if rk == 101:
        st += [{"id": rk * 10 + 3, "streamType": 2, "codec": "ac3", "displayTitle": "Commentary (AC3 5.1)"}]
    for x in st:
        if x["streamType"] == 2:
            x["selected"] = AUDSEL.get(pid, rk * 10 + 2) == x["id"]
    if rk == 101:
        st += [{"id": 1001, "streamType": 3, "codec": "srt", "language": "English",
                "displayTitle": "English (SRT)", "extendedDisplayTitle": "English (SRT)"},
               {"id": 1002, "streamType": 3, "codec": "srt", "language": "English", "key": "/library/streams/1002",
                "displayTitle": "English (SRT External)", "extendedDisplayTitle": "English (SRT External)"},
               {"id": 1003, "streamType": 3, "codec": "pgs", "language": "French", "forced": True,
                "displayTitle": "French Forced (PGS)"}]
    for x in st:
        if x["streamType"] == 3 and SUBSEL.get(pid) == x["id"]:
            x["selected"] = True
    return st


def movie(rk, title, year, vcodec, w, h, kbps, acodec="aac", container="mp4", profile="high",
          fps="24p", size=3500000000, offset=None, views=0):
    m = {"ratingKey": str(rk), "key": "/library/metadata/%d" % rk, "type": "movie",
         "title": title, "year": year, "thumb": "/library/metadata/%d/thumb/1700000000" % rk,
         "art": "/library/metadata/%d/art/1700000000" % rk, "contentRating": "PG", "rating": 7.5,
         "summary": "%s: a test film. " % title + "It goes on for a while, so the details panel has "
                    "to wrap it over several lines, as a real summary would. " * 3,
         "duration": 5400000, "viewCount": views,
         "Media": [{"container": container, "videoCodec": vcodec, "audioCodec": acodec,
                    "width": w, "height": h, "bitrate": kbps, "audioChannels": 6,
                    "videoProfile": profile, "videoFrameRate": fps,
                    "Part": [{"id": 11000 + rk, "key": "/library/parts/11/%d/file.%s" % (rk, container),
                              "file": "/media/films/%s (%d).%s" % (title, year, container),
                              "size": size}]}]}
    if offset:
        m["viewOffset"] = offset
    if rk == 101:               # the rest of the metadata, as the details of one item give it
        m.update({"guid": "plex://movie/5d776825880197001ec967c6", "studio": "Blender Foundation",
                  "librarySectionID": 1, "Collection": [{"id": 77, "filter": "collection=77", "tag": "Open Movies"}],
                  "originallyAvailableAt": "2008-04-10", "audienceRating": 8.1,
                  "Genre": [{"tag": "Animation"}, {"tag": "Comedy"}, {"tag": "Short"}],
                  "Director": [{"tag": "Sacha Goedegebure"}],
                  "Writer": [{"tag": "Sacha Goedegebure"}, {"tag": "Ton Roosendaal"}],
                  "Country": [{"tag": "Netherlands"}],
                  "Role": [{"tag": "Bunny", "role": "Himself", "thumb": "https://metadata-static.plex.tv/people/bunny.jpg"},
                           {"tag": "Frank", "role": "Flying squirrel", "thumb": "https://metadata-static.plex.tv/people/frank.jpg"},
                           {"tag": "Rinky", "role": "Red squirrel"},
                           {"tag": "Gamera", "role": "Chinchilla", "thumb": "https://metadata-static.plex.tv/people/gamera.jpg"}]})
    return m


MOVIES = [
    movie(101, "Big Buck Bunny", 2008, "h264", 1920, 1080, 5000, offset=2530000),
    movie(102, "Hevc Film", 2020, "hevc", 1920, 1080, 6000, container="mkv", profile="main 10"),
    movie(103, "Ten Bit", 2019, "h264", 1920, 1080, 6000, profile="high 10", container="mkv"),
    movie(104, "Remux", 2015, "h264", 1920, 1080, 30000, acodec="truehd", container="mkv"),
    movie(105, "Sixty", 2021, "h264", 1920, 1080, 8000, fps="60"),
    movie(106, "Dvd Rip", 1999, "mpeg2video", 720, 576, 5000, acodec="ac3", container="mpeg",
          profile="main", fps="PAL", size=4700000000, views=1),
]


def episodes(season=1):
    eps = []
    for i in range(1, 7):
        rk = {0: 230, 1: 210, 2: 220}[season] + i
        e = movie(rk, "Episode \u2018%d\u2019" % i, 2020, "h264", 1280, 720, 3000,
                  views=1 if (season == 1 and i <= 2) or season == 0 else 0)
        e.update({"type": "episode", "index": i, "parentIndex": season, "year": None,
                  "grandparentTitle": "Space Show", "grandparentArt": "/library/metadata/20/art/1",
                  "grandparentThumb": "/library/metadata/20/thumb/1", "grandparentRatingKey": "20",
                  "thumb": "/library/metadata/%d/thumb/1700000000" % rk, "duration": 2700000})
        del e["art"]
        eps.append(e)
    return eps


SHOW = {"ratingKey": "20", "key": "/library/metadata/20/children", "type": "show", "title": "Space Show",
        "year": 2020, "contentRating": "gb/12", "rating": 8.2, "childCount": 3, "leafCount": 13, "viewedLeafCount": 3,
        "summary": "Five strangers crew a salvage ship at the edge of the solar system, and find that the wreck "
                   "they came for is still broadcasting.",
        "thumb": "/library/metadata/20/thumb/1", "art": "/library/metadata/20/art/1",
        "Genre": [{"tag": "Science Fiction"}, {"tag": "Drama"}]}


# ---- a Jellyfin server, on the same port (its paths start with a capital) ----

JF_TOKEN = "JF-TOKEN"
JF_USER = "u1"
JF_QC = {"polls": 0, "secret": None}
JF_PLAYED = {"jm3"}             # item ids watched
JF_POS = {"jm1": 25300000000}   # where you got to, in ticks
JF_PREFIXES = ("/System/", "/QuickConnect/", "/Users/", "/Items/", "/Shows/", "/Videos/", "/videos/", "/Sessions/",
               "/MediaSegments/")


def jf_stream_set(iid):
    st = [{"Index": 0, "Type": "Video", "Codec": "h264", "Profile": "High", "Width": 1920, "Height": 1080,
           "BitDepth": 8, "RealFrameRate": 24.0, "BitRate": 4800000}]
    if iid == "jm2":
        st = [{"Index": 0, "Type": "Video", "Codec": "hevc", "Profile": "Main 10", "Width": 1920, "Height": 1080,
               "BitDepth": 10, "RealFrameRate": 24.0}]
    if iid.startswith("je"):
        st[0].update({"Width": 1280, "Height": 720})
    st.append({"Index": 1, "Type": "Audio", "Codec": "aac", "Channels": 2, "Language": "eng",
               "DisplayTitle": "English - AAC - Stereo", "IsDefault": True})
    if iid == "jm1":
        st += [{"Index": 2, "Type": "Audio", "Codec": "ac3", "Channels": 6, "DisplayTitle": "Commentary - AC3 - 5.1"},
               {"Index": 3, "Type": "Subtitle", "Codec": "subrip", "Language": "eng", "DisplayTitle": "English - SUBRIP"},
               {"Index": 4, "Type": "Subtitle", "Codec": "subrip", "Language": "eng", "IsExternal": True,
                "DisplayTitle": "English - SUBRIP - External"},
               {"Index": 5, "Type": "Subtitle", "Codec": "PGSSUB", "Language": "fre", "IsForced": True,
                "DisplayTitle": "French - PGSSUB - Forced"}]
    return st


def jf_item(iid, full=False):
    """One of the fake Jellyfin's items, as a BaseItemDto"""
    films = {"jm1": ("Jelly Bunny", 2008, 5400), "jm2": ("Jelly Hevc", 2020, 6000), "jm3": ("Jelly Seen", 2001, 5000)}
    ud = {"Played": iid in JF_PLAYED, "PlaybackPositionTicks": JF_POS.get(iid, 0), "PlayCount": 1 if iid in JF_PLAYED else 0}
    if iid in films:
        t, y, mins = films[iid]
        m = {"Id": iid, "Name": t, "Type": "Movie", "ProductionYear": y, "RunTimeTicks": 54000000000,
             "ImageTags": {"Primary": "pt-" + iid}, "BackdropImageTags": ["bt-" + iid], "CommunityRating": 7.4,
             "OfficialRating": "PG", "UserData": ud, "MediaType": "Video"}
    elif iid == "js1":
        m = {"Id": "js1", "Name": "Moon Show", "Type": "Series", "ProductionYear": 2022, "ChildCount": 1,
             "ImageTags": {"Primary": "pt-js1"}, "BackdropImageTags": ["bt-js1"],
             "UserData": {"Played": False, "UnplayedItemCount": 1}, "Overview": "A show about the moon."}
    elif iid == "jss1":
        m = {"Id": "jss1", "Name": "Season 1", "Type": "Season", "IndexNumber": 1, "SeriesId": "js1",
             "SeriesName": "Moon Show", "SeriesPrimaryImageTag": "pt-js1", "ChildCount": 2,
             "UserData": {"Played": False, "UnplayedItemCount": 1}}
    elif iid in ("je1", "je2", "je3"):
        n = int(iid[2])
        m = {"Id": iid, "Name": "Moon %d" % n, "Type": "Episode", "IndexNumber": n, "ParentIndexNumber": 1,
             "SeriesId": "js1", "SeriesName": "Moon Show", "SeasonId": "jss1", "SeriesPrimaryImageTag": "pt-js1",
             "RunTimeTicks": 27000000000, "ImageTags": {"Primary": "pt-" + iid}, "MediaType": "Video",
             "UserData": {"Played": iid == "je1" or iid in JF_PLAYED, "PlaybackPositionTicks": 0},
             "ParentBackdropItemId": "js1", "ParentBackdropImageTags": ["bt-js1"]}
        if iid == "je3":
            m["LocationType"] = "Virtual"
    elif iid == "jb1":
        m = {"Id": "jb1", "Name": "Jelly Collection", "Type": "BoxSet", "ChildCount": 2,
             "ImageTags": {"Primary": "pt-jb1"}, "UserData": {}}
    elif iid in ("jp1", "jp2"):
        m = {"Id": iid, "Name": "Jelly Night" if iid == "jp1" else "Jelly Songs", "Type": "Playlist",
             "MediaType": "Video" if iid == "jp1" else "Audio", "ChildCount": 2, "UserData": {}}
    else:
        return None
    if full and m["Type"] in ("Movie", "Episode"):
        m["Overview"] = "%s: a test video on a Jellyfin server." % m["Name"]
        m["Genres"] = ["Animation", "Comedy"]
        m["Studios"] = [{"Name": "Blender Foundation", "Id": "st1"}]
        m["Taglines"] = ["One big rabbit"]
        m["PremiereDate"] = "2008-04-10T00:00:00.0000000Z"
        m["People"] = [{"Name": "Bunny", "Id": "p1", "Role": "Himself", "Type": "Actor", "PrimaryImageTag": "pp1"},
                       {"Name": "Frank", "Id": "p2", "Role": "Squirrel", "Type": "Actor"},
                       {"Name": "Sacha", "Id": "p3", "Type": "Director"}]
        m["MediaSources"] = [{"Id": "ms-" + iid, "Container": "mkv" if iid != "jm1" else "mov,mp4,m4a,3gp,3g2,mj2",
                              "Path": "/media/films/%s.mkv" % m["Name"], "Size": 3500000000, "Bitrate": 5000000,
                              "DefaultAudioStreamIndex": 1, "MediaStreams": jf_stream_set(iid)}]
        if iid == "jm1":
            m["Chapters"] = [{"StartPositionTicks": 0, "Name": "Opening"},
                             {"StartPositionTicks": 50000000, "Name": "The meadow"}]
    return m


def jf_items(ids, total=None):
    its = [jf_item(i) for i in ids]
    return {"Items": its, "TotalRecordCount": len(its) if total is None else total}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def record(self, body=b""):
        u = urlsplit(self.path)
        with LOCK:
            LOG.append({"method": self.command, "path": u.path, "query": parse_qs(u.query),
                        "headers": {k: v for k, v in self.headers.items()},
                        "body": body.decode("latin-1")})

    def send(self, code, obj=None, raw=None, ctype="application/json", extra=None):
        data = raw if raw is not None else (json.dumps(obj).encode() if obj is not None else b"")
        gz = ctype == "application/json" and data and "gzip" in (self.headers.get("Accept-Encoding") or "")
        if gz:                      # as Plex does: JSON compressed when asked
            data = gzip.compress(data)
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        if gz:
            self.send_header("Content-Encoding", "gzip")
        self.send_header("Content-Length", str(len(data)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def token(self):
        q = parse_qs(urlsplit(self.path).query)
        return self.headers.get("X-Plex-Token") or (q.get("X-Plex-Token") or [None])[0]

    def jf_token(self):
        a = self.headers.get("Authorization") or ""
        if 'Token="' in a:
            return a.split('Token="', 1)[1].split('"', 1)[0]
        q = parse_qs(urlsplit(self.path).query)
        return (q.get("api_key") or q.get("ApiKey") or [None])[0]

    def jf(self, body=b""):
        """The fake Jellyfin server"""
        u = urlsplit(self.path)
        p, q, m = u.path, parse_qs(u.query), self.command
        a = self.headers.get("Authorization") or ""
        keyed = q.get("api_key") or q.get("ApiKey") or "/Images/" in p
        if not keyed and (not a.startswith("MediaBrowser ") or 'DeviceId="' not in a or 'Client="Matinee"' not in a):
            return self.send(400, {"error": "no MediaBrowser authorization"})
        js = json.loads(body) if body[:1] in (b"{", b"[") else None
        if p == "/System/Info/Public":
            return self.send(200, {"Id": "JFID", "ServerName": "Cellar", "Version": "10.10.7",
                                   "ProductName": "Jellyfin Server", "StartupWizardCompleted": True})
        if p == "/QuickConnect/Initiate" and m == "POST":
            JF_QC.update({"polls": 0, "secret": "SEC1"})
            return self.send(200, {"Secret": "SEC1", "Code": "123456", "Authenticated": False})
        if p == "/QuickConnect/Connect":
            if (q.get("secret") or [""])[0] != JF_QC["secret"]:
                return self.send(404, {})
            JF_QC["polls"] += 1
            return self.send(200, {"Secret": "SEC1", "Code": "123456", "Authenticated": JF_QC["polls"] >= 2})
        if p == "/Users/AuthenticateWithQuickConnect":
            if not js or js.get("Secret") != "SEC1" or JF_QC["polls"] < 2:
                return self.send(401, {})
            return self.send(200, {"AccessToken": JF_TOKEN, "ServerId": "JFID", "User": {"Id": JF_USER, "Name": "andrew"}})
        if p == "/Users/AuthenticateByName":
            if not js or js.get("Username") != "andrew" or js.get("Pw") != "secret":
                return self.send(401, {})
            return self.send(200, {"AccessToken": JF_TOKEN, "ServerId": "JFID", "User": {"Id": JF_USER, "Name": "andrew"}})
        if "/Images/" in p:
            return self.send(200, raw=jpeg(p + "?" + u.query), ctype="image/jpeg")
        if self.jf_token() != JF_TOKEN:
            return self.send(401, {})
        if p == "/Sessions/Logout" or p.startswith("/Sessions/Playing") or p == "/Videos/ActiveEncodings":
            return self.send(204, raw=b"")
        if p.startswith("/Users/%s/PlayedItems/" % JF_USER):
            iid = p.rsplit("/", 1)[1]
            (JF_PLAYED.add if m == "POST" else JF_PLAYED.discard)(iid)
            return self.send(200, {"Played": m == "POST"})
        if p.startswith("/Users/%s/Items/" % JF_USER) and p.endswith("/UserData"):
            iid = p.split("/")[4]
            if js and "PlaybackPositionTicks" in js:
                JF_POS[iid] = js["PlaybackPositionTicks"]
            return self.send(200, {})
        if p == "/Users/%s/Views" % JF_USER:
            return self.send(200, {"Items": [
                {"Id": "lib-m", "Name": "Jelly Films", "CollectionType": "movies", "Type": "CollectionFolder"},
                {"Id": "lib-t", "Name": "Jelly TV", "CollectionType": "tvshows", "Type": "CollectionFolder"},
                {"Id": "lib-bs", "Name": "Collections", "CollectionType": "boxsets", "Type": "CollectionFolder"},
                {"Id": "lib-mu", "Name": "Jelly Music", "CollectionType": "music", "Type": "CollectionFolder"}],
                "TotalRecordCount": 4})
        if p == "/Users/%s/Items/Resume" % JF_USER:
            return self.send(200, jf_items([i for i in ("jm1",) if JF_POS.get(i)]))
        if p == "/Shows/NextUp":
            return self.send(200, jf_items(["je2"]))
        if p == "/Users/%s/Items/Latest" % JF_USER:
            par = (q.get("ParentId") or [""])[0]
            return self.send(200, [jf_item(i) for i in ({"lib-m": ["jm2", "jm1"], "lib-t": ["je2"]}.get(par, []))])
        if p == "/Users/%s/Items" % JF_USER:
            par = (q.get("ParentId") or [""])[0]
            types = (q.get("IncludeItemTypes") or [""])[0]
            term = (q.get("searchTerm") or [""])[0].lower()
            if term:
                ids = [i for i in ("je1", "je2", "js1", "jm1", "jm2", "jm3") if term in jf_item(i)["Name"].lower()
                       or (i.startswith("je") and term in "moon show")]
                return self.send(200, jf_items(ids))
            if types == "BoxSet":
                return self.send(200, jf_items(["jb1"]))
            if types == "Playlist":
                return self.send(200, jf_items(["jp1", "jp2"]))
            if par == "lib-m":
                ids = ["jm1", "jm2", "jm3"]
                sb = (q.get("SortBy") or [""])[0]
                if sb.startswith("ProductionYear"):
                    ids.sort(key=lambda i: -jf_item(i)["ProductionYear"])
                if (q.get("Filters") or [""])[0] == "IsUnplayed":
                    ids = [i for i in ids if i not in JF_PLAYED]
                return self.send(200, jf_items(ids))
            if par == "lib-t":
                return self.send(200, jf_items(["js1"]))
            if par in ("jb1", "jp1"):
                return self.send(200, jf_items(["jm1", "jm2"]))
            return self.send(200, jf_items([]))
        if p.startswith("/Users/%s/Items/" % JF_USER) and p.endswith("/SpecialFeatures"):
            return self.send(200, [])
        if p.startswith("/Users/%s/Items/" % JF_USER) and p.count("/") == 4:
            it = jf_item(p.rsplit("/", 1)[1], full=True)
            return self.send(200, it) if it else self.send(404, {})
        if p == "/Shows/js1/Seasons":
            return self.send(200, jf_items(["jss1"]))
        if p == "/Shows/js1/Episodes":
            return self.send(200, jf_items(["je1", "je2", "je3"]))
        if p.startswith("/Items/") and p.endswith("/Similar"):
            return self.send(200, jf_items(["jm2"]))
        if p.startswith("/MediaSegments/"):
            if p.endswith("/je1"):
                return self.send(200, {"Items": [
                    {"Type": "Intro", "StartTicks": 20000000, "EndTicks": 80000000},
                    {"Type": "Commercial", "StartTicks": 90000000, "EndTicks": 100000000},
                    {"Type": "Outro", "StartTicks": 140000000, "EndTicks": 200000000}]})
            return self.send(200, {"Items": []})
        if p.startswith("/Items/") and p.endswith("/PlaybackInfo") and m == "POST":
            iid = p.split("/")[2]
            return self.send(200, {"PlaySessionId": "0123456789abcdef0123456789abcdef", "MediaSources": [
                {"Id": "ms-" + iid, "SupportsDirectPlay": False, "SupportsTranscoding": True,
                 "TranscodingUrl": "/videos/%s/master.m3u8?DeviceId=x&MediaSourceId=ms-%s&VideoCodec=h264"
                                   "&AudioCodec=aac&PlaySessionId=0123456789abcdef0123456789abcdef&ApiKey=%s"
                                   % (iid, iid, JF_TOKEN),
                 "TranscodingSubProtocol": "hls", "TranscodingContainer": "ts"}]})
        if p.endswith("/master.m3u8"):
            return self.send(200, raw=b"#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=4000000\nmain.m3u8\n",
                             ctype="application/vnd.apple.mpegurl")
        if p == "/Videos/jm1/ms-jm1/Subtitles/4/0/Stream.srt":
            return self.send(200, raw=b"1\r\n00:00:01,000 --> 00:00:04,000\r\nA jelly buck.\r\n")
        if p.startswith("/Videos/") and p.endswith("/stream"):
            rng = self.headers.get("Range")
            start = int(rng[6:].split("-")[0] or 0) if rng and rng.startswith("bytes=") else 0
            data = PART[start:]
            if start:
                return self.send(206, raw=data, ctype="video/mp4",
                                 extra={"Content-Range": "bytes %d-%d/%d" % (start, len(PART) - 1, len(PART)),
                                        "Accept-Ranges": "bytes"})
            return self.send(200, raw=data, ctype="video/mp4", extra={"Accept-Ranges": "bytes"})
        return self.send(404, {})

    def do_DELETE(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n)
        self.record(body)
        if urlsplit(self.path).path.startswith(JF_PREFIXES):
            return self.jf(body)
        self.send(404, {})

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n)
        p = urlsplit(self.path).path
        if p == "/_reset":
            with LOCK:
                LOG.clear()
                JF_PLAYED.clear()
                JF_PLAYED.add("jm3")
                JF_POS.clear()
                JF_POS["jm1"] = 25300000000
                JF_QC.update({"polls": 0, "secret": None})
            return self.send(200, {})
        self.record(body)
        if p.startswith(JF_PREFIXES):
            return self.jf(body)
        if p == "/playQueues":
            if self.token() not in SERVERS:
                return self.send(401, {})
            return self.post_playqueue(parse_qs(urlsplit(self.path).query))
        if p.startswith("/api/v2/home/users/") and p.endswith("/switch"):
            if self.token() not in ACCOUNTS:
                return self.send(401, {})
            uuid = p.split("/")[5]
            pin = (parse_qs(urlsplit(self.path).query).get("pin") or [""])[0]
            if uuid == "u-admin":
                if pin != "1234":
                    return self.send(403, {"errors": [{"code": 1041, "message": "Invalid PIN"}]})
                return self.send(201, {"id": 1, "uuid": "u-admin", "title": "Andrew", "authToken": ACCOUNT})
            if uuid == "u-kids":
                return self.send(201, {"id": 2, "uuid": "u-kids", "title": "Kids", "authToken": KID})
            return self.send(404, {})
        if p == "/api/v2/pins":
            PIN_POLLS["n"] = 0
            return self.send(201, {"id": 3141592653, "code": "ABCD", "authToken": None})
        self.send(404, {})

    def post_playqueue(self, q):
        uri = (q.get("uri") or [""])[0]
        rk = uri.rsplit("/", 1)[-1]
        return self.send(200, {"MediaContainer": {"playQueueID": 3141, "playQueueVersion": 1,
                                                  "playQueueSelectedItemID": 59265, "size": 1,
                                                  "Metadata": [{"ratingKey": rk, "playQueueItemID": 59265}]}})

    def do_PUT(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n)
        u = urlsplit(self.path)
        self.record(body)
        if self.token() not in SERVERS:
            return self.send(401, {})
        if u.path == "/:/rate":
            q = parse_qs(u.query)
            r = float(q["rating"][0])
            RATED[q["key"][0]] = r
            return self.send(200, raw=b"")
        if u.path == "/actions/removeFromContinueWatching":
            ONDECK_GONE.add(parse_qs(u.query).get("ratingKey", [""])[0])
            return self.send(200, raw=b"")
        if u.path.startswith("/library/parts/"):
            q = parse_qs(u.query)
            if "subtitleStreamID" in q:
                SUBSEL[int(u.path.rsplit("/", 1)[1])] = int(q["subtitleStreamID"][0])
            if "audioStreamID" in q:
                AUDSEL[int(u.path.rsplit("/", 1)[1])] = int(q["audioStreamID"][0])
            return self.send(200, raw=b"")
        self.send(404, {})

    def do_GET(self):
        u = urlsplit(self.path)
        p, q = u.path, parse_qs(u.query)
        if p == "/_log":
            with LOCK:
                return self.send(200, LOG)
        self.record()
        if p.startswith(JF_PREFIXES):
            return self.jf()
        if p == "/api/v2/pins/3141592653":
            PIN_POLLS["n"] += 1
            return self.send(200, {"id": 3141592653, "code": "ABCD",
                                   "authToken": ACCOUNT if PIN_POLLS["n"] >= 2 else None})
        if p.startswith("/api/v2/pins/"):
            return self.send(404, {"errors": [{"code": 1020, "message": "Code not found or expired"}]})
        if p == "/api/v2/home/users":
            if self.token() not in ACCOUNTS:
                return self.send(401, {})
            return self.send(200, {"id": 77, "name": "Youll", "users": HOME_USERS})
        if p == "/api/v2/resources":
            if self.token() not in ACCOUNTS:
                return self.send(401, {})
            base = "127.0.0.1"
            return self.send(200, [
                {"name": "Living room TV", "provides": "player", "clientIdentifier": "TV1",
                 "connections": []},
                {"name": "Attic", "provides": "server", "clientIdentifier": "MID", "owned": True,
                 "accessToken": SERVER if self.token() == ACCOUNT else KID_SERVER,
                 "connections": [
                     {"protocol": "https", "address": "10.9.9.9", "port": 32400, "local": False,
                      "relay": True, "uri": "https://10-9-9-9.relay.plex.direct:32400"},
                     {"protocol": "https", "address": "203.0.113.5", "port": 32400, "local": False,
                      "relay": False, "uri": "https://203-0-113-5.MID.plex.direct:32400"},
                     {"protocol": "https", "address": "fd00::1", "port": 32400, "local": True,
                      "relay": False, "IPv6": True, "uri": "https://fd00--1.MID.plex.direct:32400"},
                     {"protocol": "https", "address": base, "port": PORT, "local": True,
                      "relay": False, "uri": "https://127-0-0-1.MID.plex.direct:%d" % PORT},
                 ]}])
        if p == "/identity":
            return self.send(200, {"MediaContainer": {"machineIdentifier": "MID", "version": "1.41"}})
        if p == "/":
            if self.token() not in SERVERS:
                return self.send(401, {})
            return self.send(200, {"MediaContainer": {"friendlyName": "Attic"}})
        # everything below needs the server's token
        if self.token() not in SERVERS:
            return self.send(401, {})
        if p == "/library/sections":
            return self.send(200, {"MediaContainer": {"size": 3, "title1": "Plex Library", "Directory": [
                {"key": "1", "title": "Films", "type": "movie", "thumb": "/:/resources/movie.png"},
                {"key": "2", "title": "TV Programmes", "type": "show"},
                {"key": "3", "title": "Music", "type": "artist"}]}})
        if p == "/library/sections/1/all":
            films = list(MOVIES)
            srt = (q.get("sort") or [""])[0]
            if srt == "year:desc":
                films.sort(key=lambda m: -m["year"])
            elif srt == "addedAt:desc":
                films.reverse()
            if (q.get("unwatched") or [""])[0] == "1":
                films = [m for m in films if not m.get("viewCount")]
            if (q.get("collection") or [""])[0] == "77":     # the collection Open Movies
                films = MOVIES[:2] + [MOVIES[3]]
            return self.send(200, {"MediaContainer": {"size": len(films), "totalSize": len(films),
                                                      "librarySectionTitle": "Films", "title1": "Films",
                                                      "Metadata": films}})
        if p == "/library/sections/1/collections":
            return self.send(200, {"MediaContainer": {"title1": "Films", "title2": "Collections", "Metadata": [
                {"ratingKey": "500", "key": "/library/collections/500/children", "type": "collection",
                 "title": "Open Movies", "childCount": 2, "thumb": "/library/collections/500/thumb/1"}]}})
        if p == "/library/collections/500/children":
            return self.send(200, {"MediaContainer": {"title2": "Open Movies", "Metadata": MOVIES[:2]}})
        if p == "/playlists":
            return self.send(200, {"MediaContainer": {"title1": "Playlists", "Metadata": [
                {"ratingKey": "600", "key": "/playlists/600/items", "type": "playlist", "playlistType": "video",
                 "title": "Friday Night", "leafCount": 3, "composite": "/playlists/600/composite/1"},
                {"ratingKey": "601", "key": "/playlists/601/items", "type": "playlist", "playlistType": "audio",
                 "title": "Some Songs", "leafCount": 9}]}})
        if p == "/playlists/600/items":
            return self.send(200, {"MediaContainer": {"title": "Friday Night", "Metadata": MOVIES[2:5]}})
        if p == "/library/metadata/101/similar":
            return self.send(200, {"MediaContainer": {"Metadata": [MOVIES[1], MOVIES[4]]}})
        if p == "/library/metadata/101/extras":
            t = movie(701, "Big Buck Bunny Trailer", 2008, "h264", 1280, 720, 3000)
            t.update({"type": "clip", "subtype": "trailer", "duration": 33000})
            return self.send(200, {"MediaContainer": {"Metadata": [t]}})
        if p == "/library/sections/2/all":
            return self.send(200, {"MediaContainer": {"title1": "TV Programmes", "Metadata": [
                SHOW]}})
        if p == "/library/metadata/20/children":
            return self.send(200, {"MediaContainer": {"title2": "Space Show", "Metadata": [
                {"key": "/library/metadata/20/allLeaves", "title": "All episodes", "leafCount": 13,
                 "thumb": "/library/metadata/20/thumb/1"},
                {"ratingKey": "23", "key": "/library/metadata/23/children", "type": "season",
                 "title": "Specials", "index": 0, "leafCount": 1, "viewedLeafCount": 1},
                {"ratingKey": "21", "key": "/library/metadata/21/children", "type": "season",
                 "title": "Series 1", "index": 1, "leafCount": 6, "viewedLeafCount": 2},
                {"ratingKey": "22", "key": "/library/metadata/22/children", "type": "season",
                 "title": "Series 2", "index": 2, "leafCount": 6, "viewedLeafCount": 0}]}})
        if p == "/hubs/search":
            words = (q.get("query") or [""])[0].lower()
            films = [m for m in MOVIES if words in m["title"].lower()]
            eps = [e for e in episodes() if words in e["title"].lower() or words in "space show"]
            shows = [{"ratingKey": "20", "key": "/library/metadata/20/children", "type": "show",
                      "title": "Space Show", "childCount": 2, "thumb": "/library/metadata/20/thumb/1"}] \
                if words in "space show" else []
            hubs = [{"type": "episode", "hubIdentifier": "episode", "Metadata": eps},   # not in Matinee's order
                    {"type": "actor", "hubIdentifier": "actor", "Metadata": [{"tag": "Nobody"}]},
                    {"type": "movie", "hubIdentifier": "movie", "Metadata": films},
                    {"type": "show", "hubIdentifier": "show", "Metadata": shows}]
            return self.send(200, {"MediaContainer": {"size": len(hubs), "Hub": [h for h in hubs if h["Metadata"]]}})
        if p == "/library/metadata/20/allLeaves":
            return self.send(200, {"MediaContainer": {"title2": "Space Show", "Metadata": episodes()}})
        if p == "/library/streams/1002":         # the SRT file beside Big Buck Bunny
            return self.send(200, raw=b"1\r\n00:00:01,000 --> 00:00:04,000\r\nA big buck.\r\n")
        if p in ("/:/timeline", "/video/:/transcode/universal/stop", "/video/:/transcode/universal/ping"):
            return self.send(200, raw=b"")
        if p == "/library/metadata/21/children":
            return self.send(200, {"MediaContainer": {"title2": "Series 1", "Metadata": episodes()}})
        if p == "/library/sections/1/recentlyAdded":
            return self.send(200, {"MediaContainer": {"title1": "Films", "Metadata": MOVIES}})
        if p == "/library/sections/2/recentlyAdded":
            e = episodes()[5]
            return self.send(200, {"MediaContainer": {"title1": "TV Programmes", "Metadata": [e,
                {"ratingKey": "22", "key": "/library/metadata/22/children", "type": "season", "title": "Series 2",
                 "parentTitle": "Space Show", "parentRatingKey": "20", "index": 2, "leafCount": 6, "viewedLeafCount": 0,
                 "thumb": "/library/metadata/22/thumb/1"}]}})
        if p == "/library/metadata/23/children":
            return self.send(200, {"MediaContainer": {"title2": "Specials", "Metadata": episodes(0)[:1]}})
        if p == "/library/metadata/22/children":
            return self.send(200, {"MediaContainer": {"title2": "Series 2", "Metadata": episodes(2)}})
        if p == "/library/metadata/20":
            return self.send(200, {"MediaContainer": {"size": 1, "Metadata": [SHOW]}})
        if p == "/library/onDeck":
            e = movie(213, "Episode Three", 2020, "h264", 1280, 720, 3000, offset=600000)
            e.update({"type": "episode", "index": 3, "parentIndex": 1, "grandparentTitle": "Space Show",
                      "grandparentThumb": "/library/metadata/20/thumb/1", "grandparentRatingKey": "20"})
            return self.send(200, {"MediaContainer": {"title1": "On Deck",
                                                      "Metadata": [e] if "213" not in ONDECK_GONE else []}})
        if p == "/photo/:/transcode":
            return self.send(200, raw=jpeg((q.get("url") or [""])[0]), ctype="image/jpeg")
        if p.startswith("/library/metadata/") and p.count("/") == 3:
            rk = p.rsplit("/", 1)[1]
            for m in MOVIES + episodes() + episodes(2):
                if m["ratingKey"] == rk:
                    m = json.loads(json.dumps(m))
                    m["Media"][0]["Part"][0]["Stream"] = streams(m)
                    if RATED.get(rk, -1) > 0:
                        m["userRating"] = RATED[rk]
                    if q.get("includeMarkers") == ["1"] and m["type"] == "episode":
                        m["Marker"] = [{"id": 1, "type": "intro", "startTimeOffset": 2000, "endTimeOffset": 8000},
                                       {"id": 2, "type": "commercial", "startTimeOffset": 9000, "endTimeOffset": 10000},
                                       {"id": 3, "type": "credits", "startTimeOffset": 14000, "endTimeOffset": 20000,
                                        "final": True}]
                    if q.get("includeChapters") == ["1"] and rk == "101":
                        m["Chapter"] = [{"id": 1, "tag": "Opening", "index": 1, "startTimeOffset": 0, "endTimeOffset": 5000},
                                        {"id": 2, "tag": "The meadow", "index": 2, "startTimeOffset": 5000,
                                         "endTimeOffset": 10000},
                                        {"id": 3, "tag": "", "index": 3, "startTimeOffset": 10000, "endTimeOffset": 15000},
                                        {"id": 4, "tag": "The end", "index": 4, "startTimeOffset": 15000,
                                         "endTimeOffset": 5400000}]
                    return self.send(200, {"MediaContainer": {"size": 1, "Metadata": [m]}})
            return self.send(404, {})
        if p in ("/:/scrobble", "/:/unscrobble"):
            return self.send(200, raw=b"")
        if p.startswith("/library/parts/11/101/"):
            rng = self.headers.get("Range")
            start = 0
            if rng and rng.startswith("bytes="):
                start = int(rng[6:].split("-")[0] or 0)
            data = PART[start:]
            if start:
                return self.send(206, raw=data, ctype="video/mp4",
                                 extra={"Content-Range": "bytes %d-%d/%d" % (start, len(PART) - 1, len(PART)),
                                        "Accept-Ranges": "bytes"})
            return self.send(200, raw=data, ctype="video/mp4", extra={"Accept-Ranges": "bytes"})
        if p == "/video/:/transcode/universal/start.m3u8":
            return self.send(200, raw=b"#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=4000000\nsession/x/base/index.m3u8\n",
                             ctype="application/vnd.apple.mpegurl")
        self.send(404, {})


if __name__ == "__main__":
    print("part md5", hashlib.md5(PART).hexdigest(), flush=True)
    ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
