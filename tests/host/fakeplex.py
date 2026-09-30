#!/usr/bin/env python3
"""fakeplex.py PORT - a stand-in for plex.tv and a Plex Media Server, for
the host tests. It answers the requests PlexRO makes with fixed JSON, and
records every request (method, path, query, headers, body); GET /_log
returns the record as JSON, POST /_reset clears it.

The part file /library/parts/11/111/file.mp4 is 5 MB of a fixed pattern
(Range is honoured, as Plex does)."""
import json, sys, threading, hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs

PORT = int(sys.argv[1])
ACCOUNT = "ACCT-TOKEN"
SERVER = "SRV-TOKEN"
LOG = []
LOCK = threading.Lock()
PIN_POLLS = {"n": 0}
PART = bytes((i * 7 + (i >> 11)) & 255 for i in range(5 * 1024 * 1024))
# a small valid baseline JPEG would do for the tests' purposes; the bytes
# only have to arrive intact
JPEG = b"\xff\xd8\xff\xe0" + b"PLEXRO-TEST-JPEG" * 8 + b"\xff\xd9"


def movie(rk, title, year, vcodec, w, h, kbps, acodec="aac", container="mp4", profile="high",
          fps="24p", size=3500000000, offset=None, views=0):
    m = {"ratingKey": str(rk), "key": "/library/metadata/%d" % rk, "type": "movie",
         "title": title, "year": year, "thumb": "/library/metadata/%d/thumb/1700000000" % rk,
         "duration": 5400000, "viewCount": views,
         "Media": [{"container": container, "videoCodec": vcodec, "audioCodec": acodec,
                    "width": w, "height": h, "bitrate": kbps, "audioChannels": 6,
                    "videoProfile": profile, "videoFrameRate": fps,
                    "Part": [{"key": "/library/parts/11/%d/file.%s" % (rk, container),
                              "file": "/media/films/%s (%d).%s" % (title, year, container),
                              "size": size}]}]}
    if offset:
        m["viewOffset"] = offset
    return m


MOVIES = [
    movie(101, "Big Buck Bunny", 2008, "h264", 1920, 1080, 5000, offset=2530000),
    movie(102, "Hevc Film", 2020, "hevc", 1920, 1080, 6000, container="mkv", profile="main 10"),
    movie(103, "Ten Bit", 2019, "h264", 1920, 1080, 6000, profile="high 10", container="mkv"),
    movie(104, "Remux", 2015, "h264", 1920, 1080, 30000, acodec="truehd", container="mkv"),
    movie(105, "Sixty", 2021, "h264", 1920, 1080, 8000, fps="60"),
    movie(106, "Dvd Rip", 1999, "mpeg2video", 720, 576, 5000, acodec="ac3", container="mpeg",
          profile="main", fps="PAL", size=4200000000, views=1),
]


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
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def token(self):
        q = parse_qs(urlsplit(self.path).query)
        return self.headers.get("X-Plex-Token") or (q.get("X-Plex-Token") or [None])[0]

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n)
        p = urlsplit(self.path).path
        if p == "/_reset":
            with LOCK:
                LOG.clear()
            return self.send(200, {})
        self.record(body)
        if p == "/api/v2/pins":
            PIN_POLLS["n"] = 0
            return self.send(201, {"id": 4242, "code": "ABCD", "authToken": None})
        self.send(404, {})

    def do_GET(self):
        u = urlsplit(self.path)
        p, q = u.path, parse_qs(u.query)
        if p == "/_log":
            with LOCK:
                return self.send(200, LOG)
        self.record()
        if p == "/api/v2/pins/4242":
            PIN_POLLS["n"] += 1
            return self.send(200, {"id": 4242, "code": "ABCD",
                                   "authToken": ACCOUNT if PIN_POLLS["n"] >= 2 else None})
        if p.startswith("/api/v2/pins/"):
            return self.send(404, {"errors": [{"code": 1020, "message": "Code not found or expired"}]})
        if p == "/api/v2/resources":
            if self.token() != ACCOUNT:
                return self.send(401, {})
            base = "127.0.0.1"
            return self.send(200, [
                {"name": "Living room TV", "provides": "player", "clientIdentifier": "TV1",
                 "connections": []},
                {"name": "Attic", "provides": "server", "clientIdentifier": "MID", "owned": True,
                 "accessToken": SERVER,
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
            if self.token() != SERVER:
                return self.send(401, {})
            return self.send(200, {"MediaContainer": {"friendlyName": "Attic"}})
        # everything below needs the server's token
        if self.token() != SERVER:
            return self.send(401, {})
        if p == "/library/sections":
            return self.send(200, {"MediaContainer": {"size": 3, "title1": "Plex Library", "Directory": [
                {"key": "1", "title": "Films", "type": "movie", "thumb": "/:/resources/movie.png"},
                {"key": "2", "title": "TV Programmes", "type": "show"},
                {"key": "3", "title": "Music", "type": "artist"}]}})
        if p == "/library/sections/1/all":
            return self.send(200, {"MediaContainer": {"size": len(MOVIES), "totalSize": len(MOVIES),
                                                      "librarySectionTitle": "Films", "title1": "Films",
                                                      "Metadata": MOVIES}})
        if p == "/library/sections/2/all":
            return self.send(200, {"MediaContainer": {"title1": "TV Programmes", "Metadata": [
                {"ratingKey": "20", "key": "/library/metadata/20/children", "type": "show",
                 "title": "Space Show", "childCount": 2, "leafCount": 12, "viewedLeafCount": 12,
                 "thumb": "/library/metadata/20/thumb/1"}]}})
        if p == "/library/metadata/20/children":
            return self.send(200, {"MediaContainer": {"title2": "Space Show", "Metadata": [
                {"ratingKey": "21", "key": "/library/metadata/21/children", "type": "season",
                 "title": "Series 1", "index": 1, "leafCount": 6, "viewedLeafCount": 2}]}})
        if p == "/library/metadata/21/children":
            eps = []
            for i in range(1, 7):
                e = movie(210 + i, "Episode \u2018%d\u2019" % i, 2020, "h264", 1280, 720, 3000)
                e.update({"type": "episode", "index": i, "parentIndex": 1, "year": None,
                          "grandparentTitle": "Space Show"})
                eps.append(e)
            return self.send(200, {"MediaContainer": {"title2": "Series 1", "Metadata": eps}})
        if p == "/library/onDeck":
            e = movie(213, "Episode Three", 2020, "h264", 1280, 720, 3000, offset=600000)
            e.update({"type": "episode", "index": 3, "parentIndex": 1, "grandparentTitle": "Space Show",
                      "grandparentThumb": "/library/metadata/20/thumb/1"})
            return self.send(200, {"MediaContainer": {"title1": "On Deck", "Metadata": [e]}})
        if p == "/photo/:/transcode":
            return self.send(200, raw=JPEG, ctype="image/jpeg")
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
