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
the same file) and sets or resets their XP. /ranking is a public page of
their levels; with --webhook (a file holding a Discord webhook's URL) a class
reaching a new level is announced there.

POST /save?serial=... keeps a game's data (its saves, as one block; the one
before it too) in saves/ next to --file, GET /save?serial=... gives it back.

/version.txt gives the port's latest release (from GitHub, checked hourly).
/shaders/NAME is a shader storage file (shaders/ next to --file) that games
add to with POST /shaders/NAME?serial=..., merged record by record (needs
the xxhash module to check them); the consoles sending and downloading them
are counted in stats.json next to --file, shown on the page with the
players active and who is in the online's room now (relay_room.json, written
by tools/system_link_relay.py next to players.json).

With --admin-local the page and its /api/ only answer the local machine: a
TLS proxy in front of it (Caddy, giving it its https:// address), so the
password never goes over plain HTTP.
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
import time
import urllib.parse
import urllib.request

try:
    import xxhash
except ImportError:
    xxhash = None

CLASSES = 6
CLASS_NAMES = ["Batedor", "Engenheiro", "Atirador"]
# The XP each level takes (the game's rank table).
RANKS = [0, 1000, 3500, 7000, 11500, 17000, 23500, 31500, 40500, 50500, 61500, 73500, 86500,
         100500, 115500, 131500, 148500, 166500, 185500, 205000, 225500, 247000, 269500, 293000,
         317000, 342000, 368000, 394500, 422000, 450500, 479500, 509500, 540000, 571500, 603500,
         636500, 670000, 704500, 739500, 775000, 811500, 848500, 886000, 924500, 963500, 1003000,
         1043000, 1084000, 1125500, 1167000, 1208500]
RELEASES = "https://api.github.com/repos/lamarkingru/grfs-ps5/releases/latest"
SHADER_NAME = re.compile(r"^[0-9A-F]{8}\.(xsh|(fsi|fbo)\.vk\.xpso)$")


def level(xp):
    return max(i for i, need in enumerate(RANKS) if xp >= need)


def shader_records(data, shaders, size=0):
    """The valid records of a shader storage file (a header, then records
    checked by their XXH3 hash, as src/shader_seed.cpp), up to the first
    damaged one."""
    header = 8 if shaders else 12
    out, at = [], header
    while at + (12 if shaders else size) <= len(data):
        if shaders:
            count = int.from_bytes(data[at + 8:at + 12], "little") & 0x7FFFFFFF
            end = at + 12 + count * 4
            body = data[at + 12:end]
        else:
            end = at + size
            body = data[at + 8:end]
        if end > len(data) or xxhash.xxh3_64_intdigest(body) != \
                int.from_bytes(data[at:at + 8], "little"):
            break
        out.append(data[at:end])
        at = end
    return out


def pipeline_record_size(data):
    """A pipeline storage's record size: the one whose first record checks out."""
    for size in range(16, min(len(data) - 12, 4096) + 1):
        if (len(data) - 12) % size == 0 and xxhash.xxh3_64_intdigest(
                data[12 + 8:12 + size]) == int.from_bytes(data[12:12 + 8], "little"):
            return size
    return 0


def merge_shaders(old, new, name):
    """Adds the valid records of new to old."""
    shaders = name.endswith(".xsh")
    header = 8 if shaders else 12
    if len(new) <= header:
        return old
    # (another format version: the newest game's wins)
    if len(old) <= header or old[:header] != new[:header]:
        old = new[:header]
    size = 0 if shaders else pipeline_record_size(new) or pipeline_record_size(old)
    if not shaders and not size:
        return old
    kept = shader_records(old, shaders, size)
    seen = {r[:8] for r in kept}
    added = [r for r in shader_records(new, shaders, size)
             if r[:8] not in seen and not seen.add(r[:8])]
    return old[:header] + b"".join(kept + added)


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
.stats { display:grid; grid-template-columns:repeat(auto-fit, minmax(150px, 1fr)); gap:8px;
         margin-bottom:12px; }
.stat { background:var(--panel); border:1px solid var(--line); border-radius:8px; padding:10px 14px; }
.stat b { display:block; font-size:24px; font-variant-numeric:tabular-nums; }
.stat span { color:var(--muted); font-size:12px; }
table { width:100%; border-collapse:collapse; font-size:14px; }
th, td { text-align:left; padding:5px 6px; border-bottom:1px solid var(--line); }
th { color:var(--muted); font-weight:500; font-size:12px; }
td.n, th.n { text-align:right; font-variant-numeric:tabular-nums; }
.bar { position:sticky; bottom:0; background:var(--bg); padding:12px 0;
       display:flex; gap:8px; align-items:center; border-top:1px solid var(--line); }
#status { color:var(--muted); }
</style></head><body><main>
<h1>ESTATÍSTICAS</h1>
<p class="hint">Jogadores ativos, quem está na sala do online agora e os consoles que enviaram
ou baixaram o cache de shaders compartilhado (contado desde 25/10/2026).</p>
<div id="stats"></div>
<h1>JOGADORES</h1>
<p class="hint">Consoles PS5 que abriram o jogo, pelo ID do console. Banir bloqueia o online (VPS).
O XP salvo ou zerado aqui substitui o do console na próxima sincronização (ao abrir o jogo, ou em até 5 minutos).
Níveis: 1000 XP = nível 1, 3500 = 2, 7000 = 3, 11500 = 4.</p>
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
const CLASS_NAMES = ["Batedor", "Engenheiro", "Atirador"];
let players = [];
function renderPlayers() {
  const list = document.getElementById("players"); list.replaceChildren();
  players.forEach(p => {
    const total = p.xp.reduce((a, b) => a + b, 0);
    const reason = el("input", {placeholder: "Motivo do banimento", value: p.reason || ""});
    // (one XP per class, both factions alike; the game keeps six, the last three unused)
    const xp = CLASS_NAMES.map((name, i) => field("XP " + name,
                                                 el("input", {type: "number", min: 0, value: p.xp[i]})));
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
        field("XP total", el("input", {value: total, readOnly: true})),
        field("Último acesso", el("input", {value: (p.last_seen || "") + "  " + (p.ip || ""), readOnly: true}))),
      el("div", {className: "row"}, ...xp),
      el("div", {className: "row"}, field("Motivo", reason)),
      el("div", {className: "actions"},
        el("button", {textContent: "Salvar XP", onclick: () =>
          act({xp: p.xp.map((v, i) => i < xp.length
                                       ? Math.max(0, parseInt(xp[i].querySelector("input").value) || 0) : v)})}),
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
function renderStats(st) {
  const box = document.getElementById("stats"); box.replaceChildren();
  const tile = (n, label) => el("div", {className: "stat"}, el("b", {textContent: n}),
                                el("span", {textContent: label}));
  const files = Object.fromEntries(st.files.map(f => [f.name.endsWith(".xsh") ? "xsh" : "xpso", f]));
  box.append(el("div", {className: "stats"},
    tile(st.players, "jogadores"), tile(st.active_day, "ativos nas últimas 24 h"),
    tile(st.active_week, "ativos nos últimos 7 dias"), tile(st.room.length, "na sala agora"),
    tile(files.xsh ? files.xsh.records : 0, "shaders no cache"),
    tile(files.xpso ? files.xpso.records : 0, "pipelines no cache")));
  const table = (title, head, rows) => el("div", {className: "card"},
    el("label", {textContent: title}),
    rows.length ? el("table", {}, el("tr", {}, ...head.map(([h, n]) => el("th", {textContent: h, className: n ? "n" : ""}))),
                     ...rows.map(r => el("tr", {}, ...r.map((v, i) => el("td", {textContent: v, className: head[i][1] ? "n" : ""})))))
                : el("p", {className: "hint", textContent: "Ninguém."}));
  box.append(table("Na sala do online agora", [["Jogador"], ["Endereço"], ["Desde"]],
                   st.room.map(m => [m.who, m.address, m.since])));
  const who = [["Console"], ["Vezes", 1], ["Primeira"], ["Última"]];
  box.append(table("Enviaram shaders (a cada 10 min de jogo)", who,
                   st.uploads.map(e => [e.who, e.count, e.first, e.last])));
  box.append(table("Baixaram shaders (a cada abertura do jogo)", who,
                   st.downloads.map(e => [e.who, e.count, e.first, e.last])));
}
fetch("api/stats").then(r => r.json()).then(renderStats);
fetch("api/news").then(r => r.json()).then(n => { news = n; render(); });
fetch("api/donors").then(r => r.json()).then(d => { donors = d; renderDonors(); });
loadPlayers();
fetch("api/support").then(r => r.json()).then(s => SUPPORT.forEach(k => {
  const input = document.getElementById("s_" + k);
  input.value = s[k] || ""; input.oninput = () => status("não salvo"); }));
</script></body></html>
"""


RANKING = """<!doctype html>
<html lang="pt-BR"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="refresh" content="60">
<title>Ranking GR:FS PS5</title>
<style>
:root { color-scheme: dark; }
body { margin: 0; font: 16px/1.4 system-ui, sans-serif; background: #070b12; color: #e8eef5; }
main { max-width: 860px; margin: 0 auto; padding: 24px 16px; }
h1 { font-size: 24px; margin: 0 0 4px; letter-spacing: .04em; }
p { color: #93a3b6; margin: 0 0 20px; }
table { width: 100%; border-collapse: collapse; background: #0f1724; border-radius: 10px;
        overflow: hidden; }
th, td { padding: 10px 12px; text-align: left; border-bottom: 1px solid #1c2738; }
th { color: #8fd0ee; font-size: 13px; text-transform: uppercase; letter-spacing: .06em; }
td:nth-child(n+3), th:nth-child(n+3) { text-align: right; }
.on { color: #4ade80; font-size: 13px; margin-left: 6px; }
@media (max-width: 560px) { th, td { padding: 8px 6px; font-size: 14px; } }
</style></head><body><main>
<h1>Ghost Recon: Future Soldier — PS5</h1>
<p>Níveis por classe no multijogador · %COUNT% jogadores · %ONLINE% online agora</p>
<table><thead><tr><th>#</th><th>Jogador</th><th>Batedor</th><th>Engenheiro</th>
<th>Atirador</th><th>XP</th></tr></thead><tbody>%ROWS%</tbody></table>
</main></body></html>
"""


def one_line(text):
    return " ".join(str(text).split())


def escape(text):
    return text.replace("\\", "\\\\").replace("\t", "\\t").replace("\r", "").replace("\n", "\\n")


class Store:
    def __init__(self, path, webhook=None):
        self.path = path
        self.webhook = webhook
        self.version = ("", 0.0)
        folder = os.path.dirname(os.path.abspath(path))
        self.shaders_path = os.path.join(folder, "shaders")
        self.saves_path = os.path.join(folder, "saves")
        self.support_path = os.path.join(folder, "support.json")
        self.donors_path = os.path.join(folder, "donors.json")
        self.players_path = os.path.join(folder, "players.json")
        self.stats_path = os.path.join(folder, "stats.json")
        self.room_path = os.path.join(folder, "relay_room.json")
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

    def register(self, serial, name, xp, ip, choices=None, spent=None, bought=None):
        """A game's hello: its record, with the class XP it should use now,
        its decision points' choices (ID -> 1 or 2), the attachment credits
        each class spent and the attachments bought ("class:weapon" -> a
        bit mask); "replace" when the page reset them, so the game's copy is
        replaced too."""
        now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
        with self.lock:
            players = self.load_players()
            player = players.setdefault(serial, {"serial": serial, "first_seen": now,
                                                 "xp": [0] * CLASSES, "banned": False,
                                                 "reason": "", "reset": False})
            player.update(name=name or player.get("name", ""), ip=ip, last_seen=now)
            replace = bool(player.pop("choices_reset", False))
            player.setdefault("choices", {})
            player.setdefault("spent", [0, 0, 0])
            player.setdefault("bought", {})
            if player.get("reset"):
                # (the XP the page reset: the game's copy is replaced)
                player["reset"] = False
            elif xp:
                before = list(player["xp"])
                player["xp"] = [max(a, b) for a, b in zip(player["xp"], xp)]
                for i, name in enumerate(CLASS_NAMES):
                    if level(player["xp"][i]) > level(before[i]):
                        self.announce(f"🎖️ **{player.get('name') or 'Um jogador'}** subiu o "
                                      f"{name} para o nível {level(player['xp'][i])}!")
            if not replace:
                for point, choice in (choices or {}).items():
                    player["choices"].setdefault(point, choice)
                if spent:
                    player["spent"] = [max(a, b) for a, b in zip(player["spent"], spent)]
                for key, mask in (bought or {}).items():
                    player["bought"][key] = player["bought"].get(key, 0) | mask
            self.save_players(players)
            return dict(player, replace=replace)

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
                change["xp"] = [0] * CLASSES
                # (the decision points too, as the game's respec)
                player["choices"] = {}
                player["spent"] = [0, 0, 0]
                player["bought"] = {}
                player["choices_reset"] = True
            xp = change.get("xp")
            if isinstance(xp, list) and len(xp) == CLASSES:
                # (replacing the console's on its next hello)
                player["xp"] = [max(0, min(int(v), 10 ** 9)) for v in xp]
                player["reset"] = True
            self.save_players(players)
            return True

    def announce(self, text):
        """Posts to the Discord webhook, in the background."""
        if not self.webhook:
            return

        def post():
            try:
                with open(self.webhook, encoding="utf-8") as f:
                    url = f.read().strip()
                request = urllib.request.Request(
                    url, json.dumps({"content": text}).encode(),
                    {"Content-Type": "application/json", "User-Agent": "grfs-news"})
                urllib.request.urlopen(request, timeout=10).close()
            except OSError as error:
                print(f"webhook: {error}", flush=True)

        threading.Thread(target=post, daemon=True).start()

    def latest_version(self):
        """The latest release's tag, asked from GitHub at most hourly."""
        tag, checked = self.version
        if time.time() - checked > 3600:
            try:
                request = urllib.request.Request(RELEASES, headers={"User-Agent": "grfs-news"})
                with urllib.request.urlopen(request, timeout=10) as response:
                    tag = json.load(response).get("tag_name", tag)
            except (OSError, ValueError) as error:
                print(f"version: {error}", flush=True)
            self.version = (tag, time.time())
        return tag

    def ranking(self):
        """The public page of the players' levels."""
        now = datetime.datetime.now()
        rows = []
        for p in self.load_players().values():
            if p.get("banned"):
                continue
            try:
                seen = datetime.datetime.strptime(p.get("last_seen", ""), "%Y-%m-%d %H:%M")
            except ValueError:
                seen = None
            online = seen is not None and now - seen < datetime.timedelta(minutes=10)
            levels = [level(x) for x in p["xp"][:3]]
            rows.append((sum(p["xp"][:3]), p.get("name") or "?", levels, online))
        rows.sort(key=lambda r: -r[0])
        def html(text):
            return (str(text).replace("&", "&amp;").replace("<", "&lt;")
                    .replace(">", "&gt;").replace('"', "&quot;"))
        body = "".join(
            f"<tr><td>{i + 1}</td><td>{html(name)}{' <span class=on>● online</span>' if on else ''}"
            f"</td>{''.join(f'<td>{lv}</td>' for lv in levels)}"
            f"<td>{f'{xp:,}'.replace(',', '.')}</td></tr>"
            for i, (xp, name, levels, on) in enumerate(rows))
        online = sum(1 for r in rows if r[3])
        return RANKING.replace("%ROWS%", body or "<tr><td colspan=6>Ninguém ainda.</td></tr>") \
            .replace("%ONLINE%", str(online)).replace("%COUNT%", str(len(rows)))

    def registered(self, serial):
        return serial in self.load_players()

    def known(self, serial):
        player = self.load_players().get(serial)
        return player is not None and not player.get("banned")

    def count(self, kind, key):
        """A console (its serial, or "IP x" for an older game) sending or
        downloading the shared shaders."""
        now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
        with self.lock:
            try:
                with open(self.stats_path, encoding="utf-8") as f:
                    stats = json.load(f)
            except (OSError, ValueError):
                stats = {}
            entry = stats.setdefault(kind, {}).setdefault(key, {"count": 0, "first": now})
            entry["count"] += 1
            entry["last"] = now
            with open(self.stats_path + ".tmp", "w", encoding="utf-8") as f:
                json.dump(stats, f, indent=1)
            os.replace(self.stats_path + ".tmp", self.stats_path)

    def stats(self):
        """The page's numbers: players active, the room, the shared shaders."""
        try:
            with open(self.stats_path, encoding="utf-8") as f:
                stats = json.load(f)
        except (OSError, ValueError):
            stats = {}
        try:
            with open(self.room_path, encoding="utf-8") as f:
                room = json.load(f)
        except (OSError, ValueError):
            room = []
        players = self.load_players()
        now = datetime.datetime.now()

        def since(text):
            try:
                return now - datetime.datetime.strptime(text, "%Y-%m-%d %H:%M")
            except (TypeError, ValueError):
                return datetime.timedelta.max

        def named(entries):
            return sorted(({"who": players.get(key, {}).get("name") or key, **entry}
                           for key, entry in entries.items()),
                          key=lambda e: e.get("last", ""), reverse=True)

        files = []
        for name in sorted(os.listdir(self.shaders_path)) if os.path.isdir(self.shaders_path) else []:
            if not SHADER_NAME.match(name):
                continue
            data = self.shader_file(name) or b""
            shaders = name.endswith(".xsh")
            records = len(shader_records(data, shaders, 0 if shaders else pipeline_record_size(data))) \
                if xxhash else 0
            files.append({"name": name, "bytes": len(data), "records": records})
        for member in room:
            member["who"] = players.get(member.get("serial", ""), {}).get("name") or member.get("serial") or "?"
        return {
            "players": len(players),
            "active_day": sum(since(p.get("last_seen")) < datetime.timedelta(days=1)
                              for p in players.values()),
            "active_week": sum(since(p.get("last_seen")) < datetime.timedelta(days=7)
                               for p in players.values()),
            "room": room,
            "uploads": named(stats.get("uploads", {})),
            "downloads": named(stats.get("downloads", {})),
            "files": files,
        }

    def shader_file(self, name):
        try:
            with open(os.path.join(self.shaders_path, name), "rb") as f:
                return f.read()
        except FileNotFoundError:
            return None

    def add_shaders(self, name, data):
        with self.lock:
            old = self.shader_file(name) or b""
            merged = merge_shaders(old, data, name)
            if merged != old:
                os.makedirs(self.shaders_path, exist_ok=True)
                path = os.path.join(self.shaders_path, name)
                with open(path + ".tmp", "wb") as f:
                    f.write(merged)
                os.replace(path + ".tmp", path)
            return len(merged)

    def save_block(self, serial):
        try:
            with open(os.path.join(self.saves_path, serial + ".bin"), "rb") as f:
                return f.read()
        except FileNotFoundError:
            return None

    def keep_save(self, serial, data):
        with self.lock:
            os.makedirs(self.saves_path, exist_ok=True)
            path = os.path.join(self.saves_path, serial + ".bin")
            if os.path.exists(path):
                os.replace(path, path + ".1")
            with open(path + ".tmp", "wb") as f:
                f.write(data)
            os.replace(path + ".tmp", path)

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


def make_handler(store, password, admin_local=False):
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
            if admin_local and self.client_address[0] not in ("127.0.0.1", "::1"):
                self.send(403, "use the https:// address")
                return False
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
            elif self.path == "/version.txt":
                self.send(200, store.latest_version() + "\n")
            elif self.path.startswith("/save?"):
                serial = urllib.parse.parse_qs(self.path[6:]).get("serial", [""])[0]
                data = store.save_block(serial) if SERIAL.match(serial) else None
                if data is None:
                    self.send(404, "no save")
                else:
                    self.send(200, data, "application/octet-stream")
            elif self.path == "/ranking":
                self.send(200, store.ranking(), "text/html; charset=utf-8")
            elif self.path.startswith("/shaders/") and \
                    SHADER_NAME.match(urllib.parse.urlsplit(self.path).path[9:]):
                url = urllib.parse.urlsplit(self.path)
                data = store.shader_file(url.path[9:])
                if data is None:
                    self.send(404, "none yet")
                else:
                    self.send(200, data, "application/octet-stream")
                    # (once per start: games download the .xsh first)
                    if url.path.endswith(".xsh"):
                        serial = urllib.parse.parse_qs(url.query).get("serial", [""])[0]
                        store.count("downloads", serial if SERIAL.match(serial)
                                    else "IP " + self.client_address[0])
            elif self.path in ("/", "/index.html"):
                if self.authorized():
                    self.send(200, PAGE.replace("%CATEGORIES%", json.dumps(CATEGORIES)),
                              "text/html; charset=utf-8")
            elif self.path in ("/api/news", "/api/support", "/api/donors", "/api/players",
                               "/api/stats"):
                if self.authorized():
                    data = {"/api/news": store.load, "/api/support": store.load_support,
                            "/api/donors": store.load_donors, "/api/stats": store.stats,
                            "/api/players": lambda: sorted(store.load_players().values(),
                                                           key=lambda p: p.get("last_seen", ""),
                                                           reverse=True)}[self.path]()
                    self.send(200, json.dumps(data, ensure_ascii=False),
                              "application/json; charset=utf-8")
            else:
                self.send(404, "not found")

        def do_POST(self):
            # A game's hello: "key=value" lines (serial, name, xp as six
            # numbers, choices as ID:choice pairs, spent as three numbers,
            # bought as class:weapon:hex), answered alike (banned, reason, xp,
            # choices, spent, bought, replace).
            url = urllib.parse.urlsplit(self.path)
            if url.path == "/save":
                # (a ban is from the online only: the saves are kept anyway)
                serial = urllib.parse.parse_qs(url.query).get("serial", [""])[0]
                length = int(self.headers.get("Content-Length", "0"))
                if not SERIAL.match(serial) or not store.registered(serial):
                    self.send(403, "unknown console")
                elif length > 32 * 1024 * 1024:
                    self.send(413, "too large")
                else:
                    data = self.rfile.read(length)
                    if not data.startswith(b"GRSV1\n"):
                        self.send(400, "not a save")
                    else:
                        store.keep_save(serial, data)
                        self.send(200, "ok")
                return
            if url.path.startswith("/shaders/") and SHADER_NAME.match(url.path[9:]):
                # A game's shader storage, added to the one shared.
                serial = urllib.parse.parse_qs(url.query).get("serial", [""])[0]
                length = int(self.headers.get("Content-Length", "0"))
                if not xxhash:
                    self.send(503, "no xxhash")
                elif not SERIAL.match(serial) or not store.known(serial):
                    self.send(403, "unknown console")
                elif length > 16 * 1024 * 1024:
                    self.send(413, "too large")
                else:
                    size = store.add_shaders(url.path[9:], self.rfile.read(length))
                    self.send(200, f"{size}\n")
                    if url.path.endswith(".xsh"):
                        store.count("uploads", serial)
                return
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
            choices = {}
            for pair in fields.get("choices", "").split(","):
                point, _, choice = pair.partition(":")
                if point.isdigit() and choice in ("1", "2") and len(choices) < 64:
                    choices[point] = int(choice)
            try:
                spent = [max(0, min(int(v), 1000)) for v in fields.get("spent", "").split(",") if v]
            except ValueError:
                spent = []
            bought = {}
            for item in fields.get("bought", "").split(","):
                parts = item.split(":")
                if len(parts) == 3 and parts[0] in ("0", "1", "2") and parts[1].isdigit() \
                        and len(bought) < 512:
                    try:
                        bought[f"{parts[0]}:{int(parts[1])}"] = int(parts[2], 16) & (2 ** 64 - 1)
                    except ValueError:
                        pass
            player = store.register(serial, one_line(fields.get("name", ""))[:64],
                                    xp if len(xp) == CLASSES else None, self.client_address[0],
                                    choices, spent if len(spent) == 3 else None, bought)
            pairs = ",".join(f"{k}:{v}" for k, v in sorted(player["choices"].items(),
                                                            key=lambda kv: int(kv[0])))
            self.send(200, f"banned={int(player['banned'])}\nreason={player['reason']}\n"
                           f"xp={','.join(map(str, player['xp']))}\nchoices={pairs}\n"
                           f"spent={','.join(map(str, player['spent']))}\n"
                           f"bought={','.join(f'{k}:{v:x}' for k, v in sorted(player['bought'].items()))}\n"
                           f"replace={int(player['replace'])}\n")

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
    parser.add_argument("--webhook", help="file holding a Discord webhook's URL")
    parser.add_argument("--admin-local", action="store_true",
                        help="the page only for the local machine (a TLS proxy)")
    args = parser.parse_args()
    if not args.password:
        parser.error("give --password or NEWS_PASSWORD")
    store = Store(args.file, args.webhook)
    server = http.server.ThreadingHTTPServer(("", args.port), make_handler(store, args.password, args.admin_local))
    print(f"news on port {args.port}: page http://HOST:{args.port}/, game news_url "
          f"http://HOST:{args.port}/news.txt", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
