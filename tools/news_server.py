#!/usr/bin/env python3
"""Serves the Ghost Feed's news to the game, with a page to edit them.

usage: news_server.py [--port 37001] [--file news.json] [--password SECRET]

The game (option news_url = "http://HOST:PORT/news.txt") downloads
/news.txt, /support.txt for its "help keep me active" screen and /donors.txt
for its donors screen; the page at http://HOST:PORT/ (user "admin", the
password given, or the NEWS_PASSWORD environment variable) edits them, kept
in --file (and support.json and donors.json next to it).

PS5 games register at POST /player (their console's serial number, account
name and class XP), kept in players.json next to --file: the page lists
them, bans them from the online (tools/system_link_relay.py --players reads
the same file) and resets their XP.
"""

import argparse
import base64
import re
import datetime
import hmac
import http.server
import json
import os
import threading

CLASSES = 6
SERIAL = re.compile(r"^[A-Za-z0-9_-]{1,64}$")

CATEGORIES = ["", "MP", "SP", "CIS", "WEB", "MOB", "XBLA"]

PAGE = """<!doctype html>
<html lang="pt-br"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Ghost Feed</title>
<style>
:root { --bg:#10161c; --panel:#1a232c; --line:#2c3a47; --text:#e6edf3; --muted:#8b9aa8;
        --accent:#5fb3e6; --danger:#e5534b; }
* { box-sizing:border-box; }
body { margin:0; background:var(--bg); color:var(--text); font:15px system-ui,sans-serif; }
main { max-width:860px; margin:0 auto; padding:24px 16px 80px; }
h1 { font-size:22px; letter-spacing:.04em; margin:0 0 4px; }
p.hint { color:var(--muted); margin:0 0 20px; }
.card { background:var(--panel); border:1px solid var(--line); border-radius:8px;
        padding:14px; margin-bottom:12px; }
.row { display:flex; gap:8px; flex-wrap:wrap; margin-bottom:8px; }
.row > * { flex:1 1 140px; }
label { display:block; font-size:12px; color:var(--muted); margin-bottom:3px; }
input, select, textarea { width:100%; background:var(--bg); color:var(--text);
        border:1px solid var(--line); border-radius:5px; padding:7px 8px; font:inherit; }
textarea { min-height:70px; resize:vertical; }
button { background:var(--line); color:var(--text); border:0; border-radius:5px;
        padding:7px 12px; font:inherit; cursor:pointer; }
button.primary { background:var(--accent); color:#06121b; font-weight:600; }
button.danger { background:transparent; color:var(--danger); border:1px solid var(--danger); }
.actions { display:flex; gap:6px; justify-content:flex-end; }
.bar { position:sticky; bottom:0; background:var(--bg); padding:12px 0;
       display:flex; gap:8px; align-items:center; border-top:1px solid var(--line); }
#status { color:var(--muted); }
</style></head><body><main>
<h1>JOGADORES</h1>
<p class="hint">Consoles PS5 que abriram o jogo, pelo número de série. Banir bloqueia o online (VPS).</p>
<div id="players"></div>
<h1>AJUDE A ME MANTER ATIVO</h1>
<p class="hint">A tela que abre no menu principal: título, texto e um QR code do link.</p>
<div class="card">
  <div class="row"><div><label>Título</label><input id="s_title"></div></div>
  <div class="row"><div><label>Link (vira o QR code)</label><input id="s_url" placeholder="https://..."></div></div>
  <div class="row"><div><label>Texto</label><textarea id="s_text"></textarea></div></div>
</div>
<h1>DOADORES</h1>
<p class="hint">A aba DOADORES do Ghost Feed, na ordem da lista. A mensagem é opcional.</p>
<div id="donors"></div>
<p><button onclick="addDonor()">+ Novo doador</button></p>
<h1>GHOST FEED</h1>
<p class="hint">Mensagens que o jogo mostra em COMMUNITY MESSAGES. A primeira da lista
aparece primeiro. O jogo baixa de novo a cada minuto.</p>
<div id="list"></div>
<div class="bar">
  <button onclick="add()">+ Nova mensagem</button>
  <button class="primary" onclick="save()">Salvar</button>
  <span id="status"></span>
</div>
</main>
<script>
const CATEGORIES = %CATEGORIES%;
const NAMES = {"":"(nenhuma)", MP:"Multijogador", SP:"Campanha", CIS:"CIS", WEB:"Web",
               MOB:"Celular", XBLA:"XBLA"};
let news = [];
const el = (tag, props, ...kids) => { const e = document.createElement(tag);
  Object.assign(e, props || {}); kids.forEach(k => e.append(k)); return e; };
function field(label, input) { return el("div", {}, el("label", {textContent: label}), input); }
function render() {
  const list = document.getElementById("list"); list.replaceChildren();
  news.forEach((n, i) => {
    const bind = (input, key) => { input.value = n[key] || "";
      input.oninput = () => { n[key] = input.value; status(""); }; return input; };
    const cat = el("select"); CATEGORIES.forEach(c => cat.append(el("option", {value: c, textContent: NAMES[c]})));
    list.append(el("div", {className: "card"},
      el("div", {className: "row"},
        field("Data", bind(el("input", {type: "datetime-local"}), "time")),
        field("Categoria", bind(cat, "category")),
        field("Autor", bind(el("input"), "author"))),
      el("div", {className: "row"}, field("Título", bind(el("input"), "title"))),
      el("div", {className: "row"}, field("Texto", bind(el("textarea"), "text"))),
      el("div", {className: "actions"},
        el("button", {textContent: "↑", onclick: () => move(i, -1)}),
        el("button", {textContent: "↓", onclick: () => move(i, 1)}),
        el("button", {className: "danger", textContent: "Apagar", onclick: () => { news.splice(i, 1); render(); status("não salvo"); }}))));
  });
  if (!news.length) list.append(el("p", {className: "hint", textContent: "Nenhuma mensagem."}));
}
function move(i, d) { const j = i + d; if (j < 0 || j >= news.length) return;
  [news[i], news[j]] = [news[j], news[i]]; render(); status("não salvo"); }
function add() { const now = new Date(); now.setMinutes(now.getMinutes() - now.getTimezoneOffset());
  news.unshift({time: now.toISOString().slice(0, 16), category: "", author: "", title: "", text: ""});
  render(); status("não salvo"); }
function status(text) { document.getElementById("status").textContent = text; }
let players = [];
function renderPlayers() {
  const list = document.getElementById("players"); list.replaceChildren();
  players.forEach(p => {
    const total = p.xp.reduce((a, b) => a + b, 0);
    const reason = el("input", {placeholder: "Motivo do banimento", value: p.reason || ""});
    const act = async (body) => {
      const r = await fetch("api/players/" + encodeURIComponent(p.serial), {method: "PUT",
        headers: {"Content-Type": "application/json"}, body: JSON.stringify(body)});
      if (r.ok) loadPlayers(); else status("erro");
    };
    list.append(el("div", {className: "card"},
      el("div", {className: "row"},
        field("Serial", el("input", {value: p.serial, readOnly: true})),
        field("Conta", el("input", {value: p.name || "", readOnly: true})),
        field("Status", el("input", {value: p.banned ? "BANIDO" : "ativo", readOnly: true}))),
      el("div", {className: "row"},
        field("XP por classe", el("input", {value: p.xp.join(" / ") + "  (total " + total + ")", readOnly: true})),
        field("Último acesso", el("input", {value: (p.last_seen || "") + "  " + (p.ip || ""), readOnly: true}))),
      el("div", {className: "row"}, field("Motivo", reason)),
      el("div", {className: "actions"},
        el("button", {textContent: "Zerar XP", onclick: () => {
          if (confirm("Zerar o XP de " + p.serial + "?")) act({reset_xp: true}); }}),
        p.banned
          ? el("button", {textContent: "Desbanir", onclick: () => act({banned: false})})
          : el("button", {className: "danger", textContent: "Banir",
                          onclick: () => act({banned: true, reason: reason.value})}))));
  });
  if (!players.length) list.append(el("p", {className: "hint", textContent: "Nenhum jogador ainda."}));
}
function loadPlayers() { fetch("api/players").then(r => r.json()).then(p => { players = p; renderPlayers(); }); }
let donors = [];
function renderDonors() {
  const list = document.getElementById("donors"); list.replaceChildren();
  donors.forEach((d, i) => {
    const bind = (input, key) => { input.value = d[key] || "";
      input.oninput = () => { d[key] = input.value; status("não salvo"); }; return input; };
    list.append(el("div", {className: "card"},
      el("div", {className: "row"},
        field("Nome", bind(el("input"), "name")),
        field("Mensagem", bind(el("input"), "message"))),
      el("div", {className: "actions"},
        el("button", {textContent: "↑", onclick: () => moveDonor(i, -1)}),
        el("button", {textContent: "↓", onclick: () => moveDonor(i, 1)}),
        el("button", {className: "danger", textContent: "Apagar",
                      onclick: () => { donors.splice(i, 1); renderDonors(); status("não salvo"); }}))));
  });
  if (!donors.length) list.append(el("p", {className: "hint", textContent: "Nenhum doador."}));
}
function moveDonor(i, d) { const j = i + d; if (j < 0 || j >= donors.length) return;
  [donors[i], donors[j]] = [donors[j], donors[i]]; renderDonors(); status("não salvo"); }
function addDonor() { donors.unshift({name: "", message: ""}); renderDonors(); status("não salvo"); }
const SUPPORT = ["title", "url", "text"];
async function save() {
  const support = {};
  SUPPORT.forEach(k => support[k] = document.getElementById("s_" + k).value);
  const put = (path, body) => fetch(path, {method: "PUT", headers: {"Content-Type": "application/json"},
                                           body: JSON.stringify(body)});
  const results = await Promise.all([put("api/news", news), put("api/support", support),
                                     put("api/donors", donors)]);
  status(results.every(r => r.ok) ? "salvo às " + new Date().toLocaleTimeString() : "erro ao salvar");
}
fetch("api/news").then(r => r.json()).then(n => { news = n; render(); });
fetch("api/donors").then(r => r.json()).then(d => { donors = d; renderDonors(); });
loadPlayers();
fetch("api/support").then(r => r.json()).then(s => SUPPORT.forEach(k => {
  const input = document.getElementById("s_" + k);
  input.value = s[k] || ""; input.oninput = () => status("não salvo"); }));
</script></body></html>
"""


def one_line(text):
    return " ".join(str(text).split())


def escape(text):
    return text.replace("\\", "\\\\").replace("\t", "\\t").replace("\r", "").replace("\n", "\\n")


class Store:
    def __init__(self, path):
        self.path = path
        folder = os.path.dirname(os.path.abspath(path))
        self.support_path = os.path.join(folder, "support.json")
        self.donors_path = os.path.join(folder, "donors.json")
        self.players_path = os.path.join(folder, "players.json")
        self.lock = threading.Lock()

    def load(self):
        with self.lock:
            try:
                with open(self.path, encoding="utf-8") as f:
                    return json.load(f)
            except FileNotFoundError:
                return []

    def save(self, news):
        clean = []
        for n in news:
            clean.append({
                "time": str(n.get("time", ""))[:16],
                "category": n.get("category", "") if n.get("category", "") in CATEGORIES else "",
                "author": str(n.get("author", ""))[:64],
                "title": str(n.get("title", ""))[:128],
                "text": str(n.get("text", ""))[:2000],
            })
        with self.lock:
            tmp = self.path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(clean, f, ensure_ascii=False, indent=1)
            os.replace(tmp, self.path)

    def load_support(self):
        with self.lock:
            try:
                with open(self.support_path, encoding="utf-8") as f:
                    return json.load(f)
            except FileNotFoundError:
                return {}

    def save_support(self, support):
        clean = {k: str(support.get(k, ""))[:2000] for k in ("title", "url", "text")}
        clean["title"] = clean["title"][:128].replace("\n", " ")
        clean["url"] = clean["url"][:512].replace("\n", "")
        with self.lock:
            tmp = self.support_path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(clean, f, ensure_ascii=False, indent=1)
            os.replace(tmp, self.support_path)

    def load_donors(self):
        with self.lock:
            try:
                with open(self.donors_path, encoding="utf-8") as f:
                    return json.load(f)
            except FileNotFoundError:
                return []

    def save_donors(self, donors):
        clean = [{"name": one_line(d.get("name", ""))[:64], "message": one_line(d.get("message", ""))[:200]}
                 for d in donors if isinstance(d, dict) and str(d.get("name", "")).strip()]
        with self.lock:
            tmp = self.donors_path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(clean, f, ensure_ascii=False, indent=1)
            os.replace(tmp, self.donors_path)

    def load_players(self):
        try:
            with open(self.players_path, encoding="utf-8") as f:
                return json.load(f)
        except FileNotFoundError:
            return {}

    def save_players(self, players):
        tmp = self.players_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(players, f, ensure_ascii=False, indent=1)
        os.replace(tmp, self.players_path)

    def register(self, serial, name, xp, ip):
        """A game's hello: its record, with the class XP it should use now."""
        now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
        with self.lock:
            players = self.load_players()
            player = players.setdefault(serial, {"serial": serial, "first_seen": now,
                                                 "xp": [0] * CLASSES, "banned": False,
                                                 "reason": "", "reset": False})
            player.update(name=name or player.get("name", ""), ip=ip, last_seen=now)
            if player.get("reset"):
                # (the XP the page reset: the game's copy is replaced)
                player["reset"] = False
            elif xp:
                player["xp"] = [max(a, b) for a, b in zip(player["xp"], xp)]
            self.save_players(players)
            return dict(player)

    def update_player(self, serial, change):
        with self.lock:
            players = self.load_players()
            player = players.get(serial)
            if player is None:
                return False
            if "banned" in change:
                player["banned"] = bool(change["banned"])
                player["reason"] = one_line(change.get("reason", ""))[:200] if player["banned"] else ""
            if change.get("reset_xp"):
                player["xp"] = [0] * CLASSES
                player["reset"] = True
            self.save_players(players)
            return True

    def donors_text(self):
        return "".join(f"{d['name']}\t{d['message']}\n" for d in self.load_donors())

    def support_text(self):
        support = self.load_support()
        if not support.get("title"):
            return None
        return f"{support['title']}\n{support.get('url', '')}\n{support.get('text', '')}\n"

    def feed(self):
        lines = []
        for n in self.load():
            try:
                when = datetime.datetime.strptime(n["time"], "%Y-%m-%dT%H:%M")
            except ValueError:
                when = datetime.datetime.now()
            lines.append("\t".join([when.strftime("%Y-%m-%d %H:%M"), n["category"],
                                    escape(n["author"]), escape(n["title"]), escape(n["text"])]))
        return "\n".join(lines) + "\n"


def make_handler(store, password):
    expected = "Basic " + base64.b64encode(f"admin:{password}".encode()).decode()

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def send(self, code, body, content_type="text/plain; charset=utf-8", headers=()):
            data = body.encode() if isinstance(body, str) else body
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            for name, value in headers:
                self.send_header(name, value)
            self.end_headers()
            self.wfile.write(data)

        def authorized(self):
            if hmac.compare_digest(self.headers.get("Authorization", ""), expected):
                return True
            self.send(401, "login required", headers=[("WWW-Authenticate", 'Basic realm="Ghost Feed"')])
            return False

        def do_GET(self):
            if self.path == "/news.txt":
                self.send(200, store.feed())
            elif self.path == "/donors.txt":
                self.send(200, store.donors_text())
            elif self.path == "/support.txt":
                text = store.support_text()
                self.send(200 if text else 404, text or "not set")
            elif self.path in ("/", "/index.html"):
                if self.authorized():
                    self.send(200, PAGE.replace("%CATEGORIES%", json.dumps(CATEGORIES)),
                              "text/html; charset=utf-8")
            elif self.path in ("/api/news", "/api/support", "/api/donors", "/api/players"):
                if self.authorized():
                    data = {"/api/news": store.load, "/api/support": store.load_support,
                            "/api/donors": store.load_donors,
                            "/api/players": lambda: sorted(store.load_players().values(),
                                                           key=lambda p: p.get("last_seen", ""),
                                                           reverse=True)}[self.path]()
                    self.send(200, json.dumps(data, ensure_ascii=False),
                              "application/json; charset=utf-8")
            else:
                self.send(404, "not found")

        def do_POST(self):
            # A game's hello: "key=value" lines (serial, name, xp as six
            # numbers), answered alike (banned, reason, xp).
            if self.path != "/player":
                self.send(404, "not found")
                return
            length = int(self.headers.get("Content-Length", "0"))
            if length > 4096:
                self.send(413, "too large")
                return
            fields = {}
            for line in self.rfile.read(length).decode("utf-8", "replace").splitlines():
                key, _, value = line.partition("=")
                fields[key.strip()] = value.strip()
            serial = fields.get("serial", "")
            if not SERIAL.match(serial):
                self.send(400, "bad serial")
                return
            try:
                xp = [max(0, min(int(v), 10 ** 9)) for v in fields.get("xp", "").split(",") if v]
            except ValueError:
                xp = []
            player = store.register(serial, one_line(fields.get("name", ""))[:64],
                                    xp if len(xp) == CLASSES else None, self.client_address[0])
            self.send(200, f"banned={int(player['banned'])}\nreason={player['reason']}\n"
                           f"xp={','.join(map(str, player['xp']))}\n")

        def do_PUT(self):
            if self.path.startswith("/api/players/"):
                if not self.authorized():
                    return
                length = int(self.headers.get("Content-Length", "0"))
                try:
                    change = json.loads(self.rfile.read(min(length, 4096)))
                    serial = self.path[len("/api/players/"):]
                    if not isinstance(change, dict) or not store.update_player(serial, change):
                        raise ValueError
                except ValueError:
                    self.send(400, "bad request")
                    return
                self.send(200, "ok")
                return
            if self.path not in ("/api/news", "/api/support", "/api/donors"):
                self.send(404, "not found")
                return
            if not self.authorized():
                return
            length = int(self.headers.get("Content-Length", "0"))
            if length > 1 << 20:
                self.send(413, "too large")
                return
            try:
                data = json.loads(self.rfile.read(length))
                if self.path in ("/api/news", "/api/donors"):
                    if not isinstance(data, list):
                        raise ValueError
                    (store.save if self.path == "/api/news" else store.save_donors)(data)
                else:
                    if not isinstance(data, dict):
                        raise ValueError
                    store.save_support(data)
            except (ValueError, AttributeError):
                self.send(400, "bad request")
                return
            self.send(200, "ok")

        def log_message(self, fmt, *args):
            print(f"{self.address_string()} {fmt % args}", flush=True)

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=37001)
    parser.add_argument("--file", default="news.json")
    parser.add_argument("--password", default=os.environ.get("NEWS_PASSWORD"))
    args = parser.parse_args()
    if not args.password:
        parser.error("give --password or NEWS_PASSWORD")
    store = Store(args.file)
    server = http.server.ThreadingHTTPServer(("", args.port), make_handler(store, args.password))
    print(f"news on port {args.port}: page http://HOST:{args.port}/, game news_url "
          f"http://HOST:{args.port}/news.txt", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
