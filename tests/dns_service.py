#!/usr/bin/env python3
"""The DNS service of tileWin against DNS servers made up for the test.

Each check starts tilewin-dnsd unprivileged on a port of its own, with a
config, a state and a NetworkManager of its own, and fake servers that answer
every name with an address, count what they were asked and take as long as
they are told to. It checks forwarding and the cache with its shortest and
longest times, picking the fastest servers, falling over from a failing one,
answers too big for UDP, the block lists, the servers of single networks,
and prefetching: the top names are asked again without anyone asking, and
those prefetches are never counted as requests themselves.

usage: dns_service.py <tilewin-dnsd>
"""

import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

DAEMON = sys.argv[1]
failures = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def free_port():
    """A port free for UDP and TCP both, as the service takes both."""
    while True:
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        u.bind(("127.0.0.1", 0))
        port = u.getsockname()[1]
        t = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            t.bind(("127.0.0.1", port))
            return port
        except OSError:
            pass
        finally:
            u.close()
            t.close()


def encode_name(name):
    out = b""
    for label in name.split("."):
        out += bytes([len(label)]) + label.encode()
    return out + b"\0"


def query(name, qtype=1, qid=0x1234):
    return struct.pack(">HHHHHH", qid, 0x0100, 1, 0, 0, 0) + encode_name(name) + struct.pack(">HH", qtype, 1)


def parse_question(msg):
    off, labels = 12, []
    while msg[off]:
        n = msg[off]
        labels.append(msg[off + 1:off + 1 + n].decode())
        off += 1 + n
    qtype, qclass = struct.unpack(">HH", msg[off + 1:off + 5])
    return ".".join(labels), qtype, off + 5


def answers(msg):
    """(rcode, [(type, ttl, rdata)], truncated) of an answer."""
    rcode = msg[3] & 0x0F
    tc = bool(msg[2] & 0x02)
    an = struct.unpack(">H", msg[6:8])[0]
    _, _, off = parse_question(msg)
    out = []
    for _ in range(an):
        if msg[off] & 0xC0 == 0xC0:
            off += 2
        else:
            while msg[off]:
                off += 1 + msg[off]
            off += 1
        rtype, _, ttl, rdlen = struct.unpack(">HHIH", msg[off:off + 10])
        out.append((rtype, ttl, msg[off + 10:off + 10 + rdlen]))
        off += 10 + rdlen
    return rcode, out, tc


class FakeServer:
    """A DNS server on 127.0.0.1 that answers after delay seconds."""

    def __init__(self, delay=0.0, ttl=300, address=b"\x0a\x00\x00\x01", rcode=0):
        self.delay, self.ttl, self.address, self.rcode = delay, ttl, address, rcode
        self.ttls = {}       # name -> TTL of its answer
        self.big = set()     # names whose answer does not fit in UDP
        self.asked = {}      # name -> times asked
        self.lock = threading.Lock()
        # the same port for UDP and TCP: another test may hold it for TCP
        for _ in range(50):
            self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.udp.bind(("127.0.0.1", 0))
            self.port = self.udp.getsockname()[1]
            self.tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                self.tcp.bind(("127.0.0.1", self.port))
                break
            except OSError:
                self.udp.close()
                self.tcp.close()
        self.tcp.listen(8)
        threading.Thread(target=self.serve_udp, daemon=True).start()
        threading.Thread(target=self.serve_tcp, daemon=True).start()

    @property
    def text(self):
        return "127.0.0.1:%d" % self.port

    def count(self, name):
        with self.lock:
            return self.asked.get(name, 0)

    def answer(self, msg, tcp):
        name, qtype, end = parse_question(msg)
        with self.lock:
            self.asked[name] = self.asked.get(name, 0) + 1
        flags = 0x8180 | self.rcode
        records = []
        if self.rcode == 0 and qtype == 1:
            count = 40 if name in self.big else 1
            for i in range(count):
                addr = self.address if count == 1 else bytes([10, 1, i // 256, i % 256])
                records.append(b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, self.ttls.get(name, self.ttl), 4) + addr)
        if name in self.big and not tcp:
            flags |= 0x0200
            records = []
        header = msg[:2] + struct.pack(">HHHHH", flags, 1, len(records), 0, 0)
        return header + msg[12:end] + b"".join(records)

    def serve_udp(self):
        while True:
            msg, peer = self.udp.recvfrom(4096)
            def reply(msg=msg, peer=peer):
                self.udp.sendto(self.answer(msg, False), peer)
            if self.delay:
                threading.Timer(self.delay, reply).start()
            else:
                reply()

    def serve_tcp(self):
        while True:
            conn, _ = self.tcp.accept()
            data = b""
            while len(data) < 2 or len(data) < 2 + struct.unpack(">H", data[:2])[0]:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                data += chunk
            if len(data) >= 2:
                out = self.answer(data[2:], True)
                conn.sendall(struct.pack(">H", len(out)) + out)
            conn.close()


class Daemon:
    def __init__(self, config, stats=None, nm_devices=None, keyfiles=None, resolved=None,
                 kept=None, probe_via=None):
        self.work = tempfile.mkdtemp(prefix="tilewin-dns-")
        self.port = free_port()
        self.conf = os.path.join(self.work, "dns.conf")
        self.state = os.path.join(self.work, "state")
        self.run = os.path.join(self.work, "run")
        self.nm = os.path.join(self.work, "nm")
        self.keyfiles = os.path.join(self.work, "keyfiles")
        for d in (self.state, os.path.join(self.nm, "devices"), self.keyfiles):
            os.makedirs(d)
        with open(self.conf, "w") as f:
            f.write(config)
        if stats is not None:
            with open(os.path.join(self.state, "stats"), "w") as f:
                f.write(stats)
        for index, text in (nm_devices or {}).items():
            with open(os.path.join(self.nm, "devices", str(index)), "w") as f:
                f.write(text)
        for name, text in (keyfiles or {}).items():
            self.keyfile(name, text)
        self.resolved = os.path.join(self.work, "resolved-resolv.conf")
        me = "127.0.0.1:%d" % self.port
        if resolved is not None:
            with open(self.resolved, "w") as f:
                f.write(resolved.replace("{self}", me))
        if kept is not None:
            with open(os.path.join(self.state, "network-resolv.conf"), "w") as f:
                f.write(kept)
        # the check that DNS comes here asks the given server, or is left out:
        # the system resolver of the computer running the tests is not asked
        probe = ["-p", me if probe_via == "self" else probe_via] if probe_via else ["-q"]
        self.log = open(os.path.join(self.work, "log"), "w")
        self.proc = subprocess.Popen(
            [DAEMON, "-c", self.conf, "-s", self.state, "-r", self.run,
             "-l", "127.0.0.1:%d" % self.port, "-n", self.nm, "-k", self.keyfiles,
             "-R", self.resolved] + probe,
            stdout=self.log, stderr=subprocess.STDOUT)
        for _ in range(50):
            if os.path.exists(os.path.join(self.run, "status")):
                break
            time.sleep(0.1)

    def keyfile(self, name, text):
        with open(os.path.join(self.keyfiles, name), "w") as f:
            f.write(text)

    def ask(self, name, qtype=1):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(6)
        s.sendto(query(name, qtype), ("127.0.0.1", self.port))
        try:
            return answers(s.recv(65536))
        except socket.timeout:
            return None
        finally:
            s.close()

    def ask_tcp(self, name):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=6)
        q = query(name)
        s.sendall(struct.pack(">H", len(q)) + q)
        data = b""
        while len(data) < 2 or len(data) < 2 + struct.unpack(">H", data[:2])[0]:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
        s.close()
        return answers(data[2:])

    def status(self):
        try:
            with open(os.path.join(self.run, "status")) as f:
                return f.read()
        except OSError:
            return ""

    def signal(self, sig):
        self.proc.send_signal(sig)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.log.close()

    def stats(self):
        out = {}
        with open(os.path.join(self.state, "stats")) as f:
            for line in f:
                if line.startswith("#"):
                    continue
                words = line.split()
                out[(words[0], int(words[1]))] = sum(int(w.split(":")[1]) for w in words[2:])
        return out

    def show_log(self):
        with open(os.path.join(self.work, "log")) as f:
            print("--- daemon log ---\n" + f.read() + "------------------")

    def cleanup(self):
        shutil.rmtree(self.work, ignore_errors=True)


def wait_until(test, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if test():
            return True
        time.sleep(0.1)
    return test()


def today():
    t = time.time()
    return int((t + time.localtime(t).tm_gmtoff) // 86400)


def run(name, function):
    print("== " + name)
    before = len(failures)
    try:
        function()
    except Exception as e:  # a broken check must not hide the others
        check(False, "%s raised %r" % (name, e))
    return len(failures) == before


def forwarding_and_cache():
    up = FakeServer(ttl=300)
    up.ttls["short.test"] = 1
    up.ttls["long.test"] = 3600
    d = Daemon("servers %s\nfastest 0\ncache_min 3\ncache_max 2000\nprefetch no\n" % up.text)
    try:
        r = d.ask("a.test")
        check(r is not None and r[0] == 0 and r[1] and r[1][0][2] == b"\x0a\x00\x00\x01",
              "a name is answered through the server")
        r = d.ask("a.test")
        check(up.count("a.test") == 1, "the second time it comes from the cache")
        check(r and 0 < r[1][0][1] <= 300, "the cached answer keeps its TTL (%s)" % (r and r[1][0][1]))
        r = d.ask("long.test")
        check(r and r[1][0][1] <= 2000, "no answer is kept longer than the longest time")
        d.ask("short.test")
        time.sleep(1.6)
        d.ask("short.test")
        check(up.count("short.test") == 1,
              "an answer with a TTL of 1 s is kept for the shortest time of 3 s")
        time.sleep(2.0)
        d.ask("short.test")
        check(up.count("short.test") == 2, "and asked again once that is over")
        r = d.ask_tcp("tcp.test")
        check(r and r[0] == 0 and len(r[1]) == 1, "queries over TCP are answered")
    finally:
        d.stop()
        if failures:
            d.show_log()
        d.cleanup()


def cache_off_and_big_answers():
    up = FakeServer()
    up.big.add("big.test")
    d = Daemon("servers %s\ncache no\nprefetch no\n" % up.text)
    try:
        d.ask("x.test")
        d.ask("x.test")
        check(up.count("x.test") == 2, "without the cache every query goes to the server")
        r = d.ask("big.test")
        check(r and r[2], "an answer too big for UDP comes back truncated")
        r = d.ask_tcp("big.test")
        check(r and len(r[1]) == 40, "over TCP the service fetches it whole (%s records)" % (r and len(r[1])))
    finally:
        d.stop()
        d.cleanup()


def fastest_servers():
    fast = FakeServer(delay=0.0, address=b"\x0a\x00\x00\x01")
    middle = FakeServer(delay=0.08, address=b"\x0a\x00\x00\x02")
    slow = FakeServer(delay=0.4, address=b"\x0a\x00\x00\x03")
    # the slow one first, so it is not chosen just for its place in the list
    d = Daemon("servers %s %s %s\nfastest 2\nprefetch no\n" % (slow.text, middle.text, fast.text))
    try:
        tested = wait_until(lambda: "tested 0" not in d.status() and "tested" in d.status(), 6)
        check(tested, "the servers are timed when the service starts")
        status = d.status()
        uses = [line.split()[1] for line in status.splitlines() if line.startswith("use ")]
        check(sorted(uses) == sorted([fast.text, middle.text]),
              "the two fastest are used: %s" % uses)
        for i in range(10):
            d.ask("q%d.test" % i)
        slow_count = sum(slow.count("q%d.test" % i) for i in range(10))
        fast_count = sum(fast.count("q%d.test" % i) for i in range(10))
        check(slow_count == 0 and fast_count == 10,
              "queries go to the fastest, not the slow one (slow %d, fast %d)" % (slow_count, fast_count))
        times = {line.split()[1]: int(line.split()[2]) for line in status.splitlines()
                 if line.startswith("server ")}
        check(times.get(slow.text, 0) >= 300 and times.get(fast.text, 999) < 100,
              "the measured times are reported: %s" % times)
    finally:
        d.stop()
        d.cleanup()


def failover():
    broken = FakeServer(rcode=2)
    good = FakeServer()
    d = Daemon("servers %s %s\nfastest 0\nprefetch no\n" % (broken.text, good.text))
    try:
        r = d.ask("f.test")
        check(r and r[0] == 0 and r[1], "a server failing (SERVFAIL) does not fail the query")
    finally:
        d.stop()
        d.cleanup()


def block_lists():
    up = FakeServer()
    work = tempfile.mkdtemp(prefix="tilewin-lists-")
    hosts = os.path.join(work, "hosts")
    with open(hosts, "w") as f:
        f.write("# a hosts file\n127.0.0.1 localhost\n0.0.0.0 ads.example.com\n"
                "0.0.0.0 tracker.example.net # with a comment\n")
    adblock = os.path.join(work, "adblock.txt")
    with open(adblock, "w") as f:
        f.write("! adblock\n[Adblock Plus 2.0]\n||doubleclick.test^\n||path.test^/x\n@@||good.test^\n")
    plain = os.path.join(work, "plain.txt")
    with open(plain, "w") as f:
        f.write("plain.example.org\n*.wild.test\n")
    config = ("servers %s\nprefetch no\nblocklist file:%s\nblocklist file:%s\nblocklist %s\n"
              "allow ok.doubleclick.test\nblock manual.test\n" % (up.text, hosts, adblock, plain))
    d = Daemon(config)
    try:
        def blocked(name, qtype=1):
            r = d.ask(name, qtype)
            return r and r[0] == 0 and len(r[1]) == 1 and r[1][0][2] in (b"\0" * 4, b"\0" * 16)
        check(blocked("ads.example.com"), "a name of a hosts file is answered with 0.0.0.0")
        check(blocked("ads.example.com", 28), "and with :: for IPv6")
        check(blocked("tracker.example.net"), "a hosts line with a comment counts")
        check(blocked("plain.example.org"), "a name of a plain list is blocked")
        check(blocked("x.doubleclick.test"), "an adblock rule blocks the names below it")
        check(blocked("a.wild.test") and blocked("wild.test"), "a *. line blocks the name and those below")
        check(blocked("deep.manual.test"), "a name blocked by hand blocks the names below it")
        check(not blocked("sub.ads.example.com"), "a hosts line blocks only that name")
        check(not blocked("ok.doubleclick.test"), "an allowed name is not blocked")
        check(not blocked("path.test"), "adblock rules with a path are not taken")
        check(up.count("ads.example.com") == 0, "a blocked name never reaches the server")
        blocked("ads.example.com")  # a second time: counted, not listed twice

        def log():
            try:
                with open(os.path.join(d.run, "blocked")) as f:
                    return [l.split() for l in f if l.strip()]
            except OSError:
                return []
        wait_until(lambda: any(e[4] == "deep.manual.test" for e in log()), 4)
        entries = {(e[4], int(e[2])): e for e in log()}
        ads = entries.get(("ads.example.com", 1))
        check(ads is not None and int(ads[1]) == 2,
              "the log of blocked names counts ads.example.com twice: %s" % (ads,))
        check(ads is not None and ads[3] == "list", "it says a list blocked it")
        manual = entries.get(("deep.manual.test", 1))
        check(manual is not None and manual[3] == "hand", "a name blocked by hand says so")
        check(("ok.doubleclick.test", 1) not in entries, "an allowed name is not in the log")
        newest = log()[0][4] if log() else None
        check(newest == "ads.example.com", "the newest is first: %s" % newest)
        status = d.status()
        check("blocked_names 6" in status, "the status counts 6 names: %s" %
              [l for l in status.splitlines() if l.startswith("blocked_names")])
        check(any(l.startswith("list 2 ") for l in status.splitlines()),
              "the status has the count of each list")
        with open(d.conf, "w") as f:
            f.write(config + "block_answer nxdomain\n")
        d.signal(signal.SIGHUP)
        time.sleep(0.5)
        r = d.ask("ads.example.com")
        check(r and r[0] == 3, "with block_answer nxdomain a blocked name does not exist")
        with open(d.conf, "w") as f:
            f.write(config + "blocking no\n")
        d.signal(signal.SIGHUP)
        time.sleep(0.5)
        check(not blocked("ads.example.com"), "blocking can be turned off")
    finally:
        d.stop()
        d.cleanup()
        shutil.rmtree(work, ignore_errors=True)


def downloaded_lists():
    import http.server
    work = tempfile.mkdtemp(prefix="tilewin-web-")
    with open(os.path.join(work, "list.txt"), "w") as f:
        f.write("0.0.0.0 downloaded.example.com\n")

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=work, **kwargs)

        def log_message(self, *args):
            pass

    web = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
    threading.Thread(target=web.serve_forever, daemon=True).start()
    url = "http://127.0.0.1:%d/list.txt" % web.server_address[1]
    up = FakeServer()
    d = Daemon("servers %s\nprefetch no\nblocklist %s\nblocklist %s/missing.txt\n" % (up.text, url, url.rsplit("/", 1)[0]))
    try:
        def blocked():
            r = d.ask("downloaded.example.com")
            return r and r[1] and r[1][0][2] == b"\0" * 4
        check(wait_until(blocked, 10), "a block list is downloaded and used")
        status = d.status()
        check("download-failed" in status, "a list that cannot be downloaded says so")
        with open(os.path.join(work, "list.txt"), "w") as f:
            f.write("0.0.0.0 second.example.com\n")
        d.signal(signal.SIGUSR2)
        check(wait_until(lambda: (d.ask("second.example.com") or (0, []))[1][:1] ==
                         [(1, 2, b"\0" * 4)], 10), "\"update now\" downloads the lists again")
    finally:
        d.stop()
        d.cleanup()
        web.shutdown()
        shutil.rmtree(work, ignore_errors=True)


def single_networks():
    own = FakeServer(address=b"\x0a\x00\x00\x07")
    everyone = FakeServer(address=b"\x0a\x00\x00\x08")
    dhcp = FakeServer(address=b"\x0a\x00\x00\x09")
    device = "[device]\nconnection-uuid=11111111-2222\n\n[dhcp4]\ndhcp4.domain_name_servers=%s\n" % dhcp.text
    keyfile = ("[connection]\nid=Home\nuuid=11111111-2222\n\n[ipv4]\ndns=%s;\n"
               "ignore-auto-dns=true\nmethod=auto\n" % own.text)
    d = Daemon("servers %s\nfastest 0\ncache no\nprefetch no\n" % everyone.text,
               nm_devices={2: device}, keyfiles={"Home.nmconnection": keyfile})
    try:
        r = d.ask("n1.test")
        check(r and r[1] and r[1][0][2] == b"\x0a\x00\x00\x07",
              "a network with servers of its own uses them, not those for all networks")
        d.keyfile("Home.nmconnection", keyfile.replace("dns=%s;\n" % own.text, ""))
        wait_until(lambda: "source all-networks" in d.status(), 5)
        r = d.ask("n2.test")
        check(r and r[1] and r[1][0][2] == b"\x0a\x00\x00\x08",
              "without them it uses the servers for all networks")
        with open(d.conf, "w") as f:
            f.write("cache no\nprefetch no\n")
        d.signal(signal.SIGHUP)
        wait_until(lambda: "source network" in d.status(), 5)
        r = d.ask("n3.test")
        check(r and r[1] and r[1][0][2] == b"\x0a\x00\x00\x09",
              "and with none of those, the servers the network hands out")
    finally:
        d.stop()
        d.cleanup()


def networks_without_networkmanager():
    networkd = FakeServer(address=b"\x0a\x00\x00\x0b")
    before = FakeServer(address=b"\x0a\x00\x00\x0c")
    # resolved, with the service as its server for all names and the one of
    # systemd-networkd's link: the service is not asked by itself
    d = Daemon("cache no\nprefetch no\n",
               resolved="nameserver {self}\nnameserver %s\n" % networkd.text)
    try:
        r = d.ask("w1.test")
        check(r and r[1] and r[1][0][2] == b"\x0a\x00\x00\x0b",
              "without NetworkManager the servers resolved has from networkd are asked")
    finally:
        d.stop()
        d.cleanup()
    # no resolved: the resolv.conf the service replaced, kept by its helper
    d = Daemon("cache no\nprefetch no\n", kept="nameserver %s\n" % before.text)
    try:
        r = d.ask("w2.test")
        check(r and r[1] and r[1][0][2] == b"\x0a\x00\x00\x0c",
              "without resolved the servers of the resolv.conf it replaced are asked")
    finally:
        d.stop()
        d.cleanup()


def wired_check():
    up = FakeServer()
    # the "system" asks the service itself: DNS comes here
    d = Daemon("servers %s\nprefetch no\n" % up.text, probe_via="self")
    try:
        ok = wait_until(lambda: "wired 1" in d.status(), 6)
        check(ok, "the service sees its own look-up come to it (wired 1)")
        check(not any(n.endswith("tilewin-dns.example") for n in up.asked),
              "its look-up is answered by the service, never sent on")
        check("today 0 " in d.status(), "and is not counted as a query of an app")
    finally:
        d.stop()
        d.cleanup()
    # the "system" asks another server: DNS goes elsewhere
    other = FakeServer()
    d = Daemon("servers %s\nprefetch no\n" % up.text, probe_via=other.text)
    try:
        ok = wait_until(lambda: "wired 0" in d.status(), 15)
        check(ok, "a look-up that never comes says DNS goes elsewhere (wired 0)")
    finally:
        d.stop()
        d.cleanup()


def prefetching():
    up = FakeServer(ttl=3)
    day = today()
    # 100 names asked for by apps: name00 most, name99 least; and earlier
    # than the window, a name asked for a lot but long ago
    lines = ["name%02d.test 1 %d:%d" % (i, day, 1000 - i) for i in range(100)]
    lines.append("old.test 1 %d:99999" % (day - 10))
    d = Daemon("servers %s\nfastest 0\nprefetch yes\nprefetch_percent 5\nprefetch_days 7\n"
               "prefetch_skip name01.test\nprefetch_add always.test\n" % up.text,
               stats="\n".join(lines) + "\n")
    try:
        prefetched = wait_until(lambda: up.count("name00.test") >= 1, 8)
        check(prefetched, "the top name is fetched without anyone asking")
        with open(os.path.join(d.run, "prefetch")) as f:
            names = [line.split()[1] for line in f if line.strip()]
        expected = ["name00.test", "name02.test", "name03.test", "name04.test",
                    "always.test"]
        check(sorted(set(names)) == sorted(expected),
              "the top 5%% of the last 7 days, without the skipped, with the added: %s" % names)
        check(up.count("name05.test") == 0, "a name below the top 5% is not prefetched")
        check(up.count("old.test") == 0, "a name asked for only before the 7 days is not")
        # the answers live 3 s: in the last tenth of that they are fetched again
        refreshed = wait_until(lambda: up.count("name00.test") >= 2, 40)
        check(refreshed, "a prefetched answer is fetched again before it runs out")
        r = d.ask("name00.test")
        check(r and r[0] == 0, "an app gets the prefetched answer")
        d.ask("name50.test")
        d.stop()
        stats = d.stats()
        check(stats.get(("name00.test", 1)) == 1001,
              "only the app's query was counted, not the %d prefetches (count %s)"
              % (up.count("name00.test"), stats.get(("name00.test", 1))))
        check(stats.get(("name02.test", 1)) == 998, "prefetches alone count nothing")
        check(("always.test", 1) not in stats, "a name prefetched only is never counted")
        check(stats.get(("name50.test", 1)) == 951, "queries of apps are counted")
    finally:
        d.stop()
        d.cleanup()


checks = [
    ("forwarding and cache", forwarding_and_cache),
    ("cache off, big answers", cache_off_and_big_answers),
    ("fastest servers", fastest_servers),
    ("failover", failover),
    ("block lists", block_lists),
    ("downloaded lists", downloaded_lists),
    ("single networks", single_networks),
    ("networks without NetworkManager", networks_without_networkmanager),
    ("wired check", wired_check),
    ("prefetching", prefetching),
]
only = os.environ.get("DNS_TEST_ONLY")
for title, function in checks:
    if not only or only in title:
        run(title, function)
if failures:
    print("%d failed" % len(failures))
    sys.exit(1)
print("all passed")
