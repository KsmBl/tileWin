#!/usr/bin/env python3
"""tilewin-portal against login pages made up for the test.

Each check starts a web server that plays a public Wi-Fi: its check page
(what tilewin-portal asks to know whether the internet is there) answers with
the login page until the way in was taken, and "NetworkManager is online"
after. Then "tilewin-portal check" is run against it:

 - a network without a login page is online;
 - a hotel's page behind a redirect and a meta refresh, with a cookie, a
   hidden token, the terms to tick, a newsletter to leave unticked and a
   Decline beside the Accept button, is accepted;
 - a page that leads on by a link to a second page with the form;
 - a network that answers the check page with its own page, not a redirect;
 - a page that wants an email is opened for the person (xdg-open), not sent;
 - a page made by scripts fails without a browser, and with Chromium installed
   is accepted in it;
 - with "accept no" in portal.conf nothing is sent, the page is only shown.

usage: portal.py <tilewin-portal>
"""

import http.server
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import urllib.parse

PORTAL = sys.argv[1]
failures = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


class Network:
    """A web server playing a Wi-Fi with a login page of the given pages."""

    def __init__(self, pages, redirect=True):
        self.pages = pages  # path -> function(handler, query, form) -> (code, headers, body)
        self.redirect = redirect
        self.online = False
        self.posts = []
        self.requests = []
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def answer(self, form):
                url = urllib.parse.urlsplit(self.path)
                query = urllib.parse.parse_qs(url.query, keep_blank_values=True)
                outer.requests.append((self.command, self.path))
                if url.path == "/check":
                    if outer.online:
                        return self.send(200, {}, "NetworkManager is online\n")
                    if outer.redirect:
                        return self.send(302, {"Location": outer.base + "/start?from=check"}, "")
                    return outer.pages["/start"](self, query, form)
                page = outer.pages.get(url.path)
                if not page:
                    return self.send(404, {}, "no such page")
                return page(self, query, form)

            def send(self, code, headers, body):
                data = body.encode()
                self.send_response(code)
                for k, v in headers.items():
                    self.send_header(k, v)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                self.answer({})

            def do_POST(self):
                length = int(self.headers.get("Content-Length", 0))
                form = urllib.parse.parse_qs(self.rfile.read(length).decode(), keep_blank_values=True)
                outer.posts.append((self.path, form, self.headers.get("Cookie", "")))
                self.answer(form)

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.base = "http://127.0.0.1:%d" % self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def stop(self):
        self.server.shutdown()


def run(network, conf=None, browser="none"):
    """Runs tilewin-portal check; (status, output, the URLs xdg-open got)."""
    work = tempfile.mkdtemp(prefix="tilewin-portal-")
    try:
        os.makedirs(os.path.join(work, "bin"))
        os.makedirs(os.path.join(work, "config", "tileWin"))
        opened = os.path.join(work, "opened")
        with open(os.path.join(work, "bin", "xdg-open"), "w") as f:
            f.write("#!/bin/sh\necho \"$1\" >> %s\n" % opened)
        os.chmod(os.path.join(work, "bin", "xdg-open"), 0o755)
        if conf:
            with open(os.path.join(work, "config", "tileWin", "portal.conf"), "w") as f:
                f.write(conf)
        env = dict(os.environ)
        env.update({
            "PATH": os.path.join(work, "bin") + ":" + env.get("PATH", ""),
            "XDG_CONFIG_HOME": os.path.join(work, "config"),
            "TILEWIN_PORTAL_CHECK_URL": network.base + "/check",
            "TILEWIN_PORTAL_BROWSER": browser,
        })
        env.pop("TILEWIN_PORTAL_NOTIFY", None)
        p = subprocess.run([PORTAL, "check", "Testnet"], env=env, capture_output=True,
                           text=True, timeout=120)
        urls = open(opened).read().split() if os.path.exists(opened) else []
        return p.returncode, p.stdout + p.stderr, urls
    finally:
        shutil.rmtree(work, ignore_errors=True)


def online_page(handler, query, form):
    return handler.send(200, {}, "<h1>You are online</h1>")


# ---- no login page
def no_portal():
    net = Network({})
    net.online = True
    try:
        status, out, urls = run(net)
        check(status == 0 and out.startswith("online"), "a network without a login page is online")
        check(not urls, "and nothing is opened")
    finally:
        net.stop()


# ---- a hotel: redirect, meta refresh, cookie, token, terms, newsletter
def hotel():
    net = None

    def start(h, query, form):
        return h.send(200, {}, '<html><head><meta http-equiv="refresh" content="0; url=/welcome?lang=en">'
                      '</head><body>Redirecting...</body></html>')

    def welcome(h, query, form):
        return h.send(200, {"Set-Cookie": "session=abc123; Path=/"},
                      "<html><head><title>Grand Hotel Wi-Fi</title></head><body>"
                      "<p>Welcome to the free Wi-Fi of the Grand Hotel.</p>"
                      "<form method='post' action='/login'>"
                      "<input type='hidden' name='token' value='t0k&amp;en'>"
                      "<label><input type='checkbox' name='terms' value='yes'> I have read and accept "
                      "the <a href='/terms'>terms of use</a></label><br>"
                      "<label><input type='checkbox' name='news' value='1'> Send me offers and the "
                      "newsletter</label><br>"
                      "<button type='submit' name='do' value='decline'>Decline</button>"
                      "<button type='submit' name='do' value='accept' class='btn'>Accept &amp; connect</button>"
                      "</form><a href='/help'>Help</a></body></html>")

    def login(h, query, form):
        ok = (form.get("token") == ["t0k&en"] and form.get("terms") == ["yes"] and
              "news" not in form and form.get("do") == ["accept"] and
              "session=abc123" in h.headers.get("Cookie", ""))
        if ok:
            net.online = True
            return h.send(302, {"Location": "/done"}, "")
        return h.send(403, {}, "<p>Please accept the terms.</p>")

    net = Network({"/start": start, "/welcome": welcome, "/login": login, "/done": online_page})
    try:
        status, out, urls = run(net)
        check(status == 0 and "accepted" in out, "a hotel's page is accepted: %s" % out.strip()[-300:])
        sent = net.posts[-1][1] if net.posts else {}
        check("news" not in sent, "the newsletter stays unticked: %s" % sent)
        check(sent.get("do") == ["accept"], "Accept is pressed, not Decline")
        check(not urls, "nothing is opened")
    finally:
        net.stop()


# ---- two pages: a link on, then a form sent by GET
def two_steps():
    net = None

    def start(h, query, form):
        return h.send(200, {}, "<h2>Kostenloses WLAN</h2><p>Willkommen!</p>"
                      "<a href='/impressum'>Impressum</a> <a class='button' href='step2.html'>Weiter</a>")

    def step2(h, query, form):
        return h.send(200, {}, "<form action='/go'><input type=hidden name=mac value='aa:bb'>"
                      "<input type=checkbox name=agb required> Ich akzeptiere die AGB"
                      "<input type=submit value='Jetzt kostenlos surfen'></form>")

    def go(h, query, form):
        if query.get("agb") == ["on"] and query.get("mac") == ["aa:bb"]:
            net.online = True
        return h.send(200, {}, "<p>Viel Spaß!</p>")

    net = Network({"/start": start, "/step2.html": step2, "/go": go})
    try:
        status, out, urls = run(net)
        check(status == 0 and "accepted" in out, "a page of two steps in German is accepted")
        check(not any(path.startswith("/impressum") for _, path in net.requests),
              "the imprint link is not followed")
    finally:
        net.stop()


# ---- the check page answered with the login page itself
def injected():
    net = None

    def start(h, query, form):
        return h.send(200, {}, "<form method=post action='http://%s/accept'>"
                      "<input type=submit value='Connect'></form>" % h.headers["Host"])

    def accept(h, query, form):
        net.online = True
        return h.send(200, {}, "ok")

    net = Network({"/start": start, "/accept": accept}, redirect=False)
    try:
        status, out, urls = run(net)
        check(status == 0 and "accepted" in out,
              "a login page given instead of the check page is accepted")
    finally:
        net.stop()


# ---- an email wanted: opened for the person
def wants_email():
    def start(h, query, form):
        return h.send(200, {}, "<form method=post action=/reg><input type=email name=mail required "
                      "placeholder='Your e-mail'><input type=checkbox name=tos> I agree"
                      "<button>Connect</button></form>")

    net = Network({"/start": start})
    try:
        status, out, urls = run(net)
        check(status == 2 and "needs-you" in out, "a page wanting an email is left to the person")
        check(not net.posts, "nothing is sent to it")
        check(urls and urls[0].startswith(net.base + "/start"), "it is opened: %s" % urls)
    finally:
        net.stop()


# ---- made by scripts
SCRIPTED = ("<html><body><div id=app></div><script>"
            "const b = document.createElement('div'); b.className = 'btn-primary';"
            "b.textContent = 'Get online'; b.setAttribute('role', 'button');"
            "b.onclick = () => fetch('/api/accept', {method: 'POST', body: 'x'})"
            "  .then(() => { document.body.textContent = 'Online!'; });"
            "const c = document.createElement('input'); c.type = 'checkbox'; c.id = 't';"
            "const l = document.createElement('label'); l.htmlFor = 't'; l.textContent = 'I accept the terms';"
            "document.getElementById('app').append(c, l, b);"
            "</script></body></html>")


def scripted(browser):
    net = None

    def start(h, query, form):
        return h.send(200, {}, SCRIPTED)

    def api(h, query, form):
        net.online = True
        return h.send(200, {}, "{}")

    net = Network({"/start": start, "/api/accept": api})
    try:
        status, out, urls = run(net, browser=browser)
        if browser == "none":
            check(status == 1 and "failed" in out, "a page made by scripts fails without a browser")
            check(urls and urls[0].startswith(net.base), "and is opened for the person")
        else:
            check(status == 0 and "accepted" in out,
                  "a page made by scripts is accepted in %s: %s" % (browser, out.strip()[-400:]))
    finally:
        net.stop()


# ---- switched off
def switched_off():
    def start(h, query, form):
        return h.send(200, {}, "<form method=post action=/ok><input type=submit value=Accept></form>")

    net = Network({"/start": start, "/ok": online_page})
    try:
        status, out, urls = run(net, conf="# mine\naccept no\n")
        check(status == 2 and not net.posts, "with accept no nothing is sent")
    finally:
        net.stop()


def find_chromium():
    for name in ("chromium", "chromium-browser", "google-chrome-stable", "google-chrome"):
        path = shutil.which(name)
        if path:
            return path
    return None


no_portal()
hotel()
two_steps()
injected()
wants_email()
scripted("none")
switched_off()
chromium = find_chromium()
if chromium:
    scripted(chromium)
else:
    print("skip a page made by scripts in Chromium: none is installed")
if failures:
    print("%d failed" % len(failures))
    sys.exit(1)
print("all passed")
