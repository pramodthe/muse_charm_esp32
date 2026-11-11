# SPDX-License-Identifier: MIT
"""Muse's web app: a conversation with Muse about the bigger things you hand it.

    GET  /results             the app (one page; it holds no data itself)
    GET  /results.json        the jobs, newest first, without their write-ups
    GET  /results/<id>.json   one job with its write-up
    GET  /app.json            Muse's state, timers and memory
    POST /ask                 {"task": "..."} starts a background job
    POST /memory              {"add": "..."} or {"forget": "..."}
    GET  /avatar.png          your own picture for Muse, if companion/avatar.png exists

Online, everything needs the server's token, except that MUSE_RESULTS_PUBLIC
"true" lets anyone who has the address read the jobs (the two /results
addresses). The page keeps the token in the browser: `muse_companion.py
results` opens the page with the token after the "#" of the address (that part
is never sent to a server), and the page stores it and removes it from the
address bar. On your own computer, without a token, the page is open and only
this computer may post.

It is laid out as a chat, the way personal-agent apps such as OpenMuse
(https://github.com/CopilotKit/openmuse) are: your requests on the right,
Muse's answers on the left with a card for each write-up, a message box at the
bottom. Two more tabs hold ideas to start from and what Muse remembers.
"""

PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="robots" content="noindex">
<meta name="theme-color" content="#f6f7f7" media="(prefers-color-scheme: light)">
<meta name="theme-color" content="#101415" media="(prefers-color-scheme: dark)">
<title>Muse</title>
<style>
  :root {
    --canvas: #f6f7f7; --card: #ffffff; --text: #131a1c; --muted: #677177; --line: #e6e9e9;
    --theirs: #eceeef; --mine: #d3f1e8; --brand: #0f766e; --brand-ink: #ffffff; --tile: #e1f3ef;
    --danger: #b3261e; --danger-bg: #fcedec; --ok: #1f9d6b; --shade: rgba(19, 26, 28, 0.36);
    --lift: 0 1px 2px rgba(19, 26, 28, 0.05), 0 8px 28px rgba(19, 26, 28, 0.07);
  }
  @media (prefers-color-scheme: dark) {
    :root { --canvas: #101415; --card: #1a2022; --text: #eef1f1; --muted: #98a2a6; --line: #2a3134;
            --theirs: #232a2d; --mine: #14534c; --brand: #4fd1c5; --brand-ink: #062b28; --tile: #173b37;
            --danger: #ff9b93; --danger-bg: #3a1f1d; --ok: #5fd39e; --shade: rgba(0, 0, 0, 0.6);
            --lift: 0 1px 2px rgba(0, 0, 0, 0.3), 0 8px 28px rgba(0, 0, 0, 0.35); }
  }
  * { box-sizing: border-box; }
  html { -webkit-text-size-adjust: 100%; }
  body { margin: 0; background: var(--canvas); color: var(--text);
         font: 15.5px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, "Helvetica Neue", sans-serif; }
  button, input, textarea { font: inherit; color: inherit; }
  button { cursor: pointer; border: 0; background: none; padding: 0; }
  svg { width: 20px; height: 20px; fill: none; stroke: currentColor; stroke-width: 1.8; stroke-linecap: round;
        stroke-linejoin: round; flex: none; }
  :focus-visible { outline: 2px solid var(--brand); outline-offset: 2px; }
  [hidden] { display: none !important; }
  .app { max-width: 760px; margin: 0 auto; min-height: 100vh; min-height: 100dvh; display: flex; flex-direction: column; }

  /* top bar: who you are talking to, and the tabs */
  .bar { position: sticky; top: 0; z-index: 3; display: flex; align-items: center; gap: 12px; padding: 14px 20px;
         background: color-mix(in srgb, var(--canvas) 88%, transparent); -webkit-backdrop-filter: blur(14px);
         backdrop-filter: blur(14px); }
  .face { position: relative; width: 46px; height: 46px; flex: none; }
  .face img, .face b { width: 46px; height: 46px; border-radius: 50%; display: grid; place-items: center;
                       background: var(--tile); color: var(--brand); font-size: 21px; font-weight: 700; object-fit: cover; }
  .face i { position: absolute; right: 0; bottom: 0; width: 13px; height: 13px; border-radius: 50%;
            background: var(--muted); border: 2.5px solid var(--canvas); }
  .face i.on { background: var(--ok); }
  .who { min-width: 0; flex: 1; }
  .who strong { display: block; font-size: 17px; letter-spacing: -0.01em; line-height: 1.2; }
  .who span { display: block; font-size: 13px; color: var(--muted); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .tabs { display: flex; gap: 2px; background: var(--theirs); padding: 4px; border-radius: 999px; }
  .tabs button { display: flex; align-items: center; gap: 7px; padding: 7px 14px; border-radius: 999px; font-weight: 600;
                 font-size: 14px; color: var(--muted); }
  .tabs button svg { width: 17px; height: 17px; }
  .tabs button[aria-current="page"] { background: var(--card); color: var(--text); box-shadow: 0 1px 2px rgba(19, 26, 28, 0.08); }

  main { flex: 1; padding: 8px 20px 20px; min-width: 0; }
  h1 { font-size: 22px; letter-spacing: -0.02em; margin: 14px 0 4px; }
  h2 { font-size: 14px; color: var(--muted); font-weight: 600; margin: 26px 4px 8px; }
  .lede { color: var(--muted); margin: 0 0 6px; max-width: 54ch; }

  /* the conversation */
  .day { text-align: center; margin: 22px 0 14px; }
  .day span { font-size: 12px; font-weight: 600; color: var(--muted); background: var(--theirs); padding: 4px 12px; border-radius: 999px; }
  .msg { display: flex; flex-direction: column; margin: 6px 0; }
  .msg.me { align-items: flex-end; margin-top: 18px; }
  .msg.muse { align-items: flex-start; }
  .bubble { padding: 11px 16px; border-radius: 22px; overflow-wrap: anywhere; max-width: 88%; }
  .me .bubble { background: var(--mine); border-bottom-right-radius: 8px; }
  .muse .bubble { background: var(--theirs); border-bottom-left-radius: 8px; }
  .bubble.bad { background: var(--danger-bg); color: var(--danger); }
  .time { font-size: 11.5px; color: var(--muted); margin: 4px 6px 0; }
  .doing { display: flex; align-items: center; gap: 9px; color: var(--muted); font-size: 14px; margin: 8px 6px; }
  .doing svg { width: 17px; height: 17px; }
  .dots { display: inline-flex; gap: 4px; padding: 15px 16px; }
  .dots i { width: 7px; height: 7px; border-radius: 50%; background: var(--muted); animation: hop 1.3s ease-in-out infinite; }
  .dots i:nth-child(2) { animation-delay: 0.16s; } .dots i:nth-child(3) { animation-delay: 0.32s; }
  @keyframes hop { 0%, 60%, 100% { transform: translateY(0); opacity: 0.45; } 30% { transform: translateY(-5px); opacity: 1; } }
  @media (prefers-reduced-motion: reduce) { .dots i { animation: none; } }
  .card { background: var(--card); border: 1px solid var(--line); border-radius: 22px; padding: 16px; width: min(100%, 440px);
          margin-top: 6px; }
  .card .head { display: flex; gap: 12px; align-items: center; }
  .tile { width: 42px; height: 42px; border-radius: 13px; background: var(--tile); color: var(--brand); display: grid;
          place-items: center; flex: none; }
  .card .head strong { display: block; line-height: 1.3; overflow-wrap: anywhere; }
  .card .head span { font-size: 12.5px; color: var(--muted); }
  .wide { display: flex; align-items: center; justify-content: center; gap: 8px; width: 100%; margin-top: 14px; padding: 10px;
          border-radius: 14px; background: var(--theirs); font-weight: 600; font-size: 14.5px; }
  .wide:hover { background: var(--tile); }
  .chip { display: inline-flex; align-items: center; gap: 6px; margin-top: 8px; padding: 7px 14px; border-radius: 999px;
          background: var(--card); border: 1px solid var(--line); font-weight: 600; font-size: 14px; }
  .welcome { text-align: center; padding: 9vh 10px 20px; }
  .welcome .tile { width: 60px; height: 60px; border-radius: 20px; margin: 0 auto 16px; }
  .welcome .tile svg { width: 28px; height: 28px; }
  .welcome h1 { margin: 0 0 6px; }
  .welcome p { color: var(--muted); max-width: 40ch; margin: 0 auto 16px; }
  .starters { display: flex; flex-direction: column; gap: 8px; max-width: 400px; margin: 0 auto; }
  .starters button { background: var(--card); border: 1px solid var(--line); border-radius: 16px; padding: 11px 14px; text-align: left; }
  .link { color: var(--brand); font-weight: 600; text-decoration: underline; text-underline-offset: 3px; }

  /* the message box */
  .compose { position: sticky; bottom: 0; z-index: 2; padding: 6px 20px 18px;
             background: linear-gradient(to bottom, transparent, var(--canvas) 26%); }
  .box { display: flex; align-items: flex-end; gap: 8px; background: var(--card); border: 1px solid var(--line);
         border-radius: 28px; padding: 7px 7px 7px 18px; box-shadow: var(--lift); }
  .box:focus-within { border-color: var(--brand); }
  .box textarea { flex: 1; min-width: 0; border: 0; outline: 0; background: none; resize: none; padding: 8px 0; line-height: 1.4;
                  max-height: 8.6em; }
  .send { width: 40px; height: 40px; border-radius: 50%; background: var(--brand); color: var(--brand-ink); display: grid;
          place-items: center; flex: none; }
  .send:disabled { background: var(--theirs); color: var(--muted); cursor: default; }
  .compose .hint { font-size: 12.5px; color: var(--muted); margin: 8px 12px 0; text-align: center; min-height: 1em; }

  /* lists: ideas and memory */
  .group { background: var(--card); border: 1px solid var(--line); border-radius: 22px; overflow: hidden; }
  .item { display: flex; align-items: center; gap: 13px; width: 100%; padding: 13px 16px; text-align: left;
          border-top: 1px solid var(--line); }
  .item:first-child { border-top: 0; }
  button.item:hover { background: var(--canvas); }
  .item .words { flex: 1; min-width: 0; overflow-wrap: anywhere; }
  .item .words b { display: block; font-weight: 600; }
  .item .words span { color: var(--muted); font-size: 13.5px; }
  .item > svg { color: var(--muted); width: 18px; height: 18px; }
  .item .tile { width: 38px; height: 38px; border-radius: 12px; }
  .round { width: 34px; height: 34px; border-radius: 50%; display: grid; place-items: center; color: var(--muted); flex: none; }
  .round:hover { background: var(--danger-bg); color: var(--danger); }
  .round svg { width: 17px; height: 17px; }
  .addrow { display: flex; gap: 8px; margin: 14px 0 6px; }
  .field { flex: 1; min-width: 0; border: 1px solid var(--line); border-radius: 999px; background: var(--card); padding: 10px 16px; }
  .field:focus { border-color: var(--brand); outline: none; }
  .solid { background: var(--brand); color: var(--brand-ink); border-radius: 999px; padding: 10px 18px; font-weight: 600; }
  .status { font-size: 13px; color: var(--muted); margin: 6px 6px 10px; min-height: 1.2em; }
  .status.bad { color: var(--danger); }
  .empty { color: var(--muted); text-align: center; padding: 26px 20px; }
  .signin { background: var(--card); border: 1px solid var(--line); border-radius: 24px; padding: 24px; margin: 8vh auto 0; max-width: 440px; }
  .signin h1 { margin-top: 0; }
  code { font-family: ui-monospace, "SF Mono", Menlo, monospace; font-size: 0.86em; background: var(--theirs); padding: 1px 5px; border-radius: 5px; }

  /* reading a write-up */
  .shade { position: fixed; inset: 0; z-index: 10; background: var(--shade); display: flex; align-items: center; justify-content: center; padding: 24px; }
  .sheet { background: var(--card); border-radius: 26px; width: 100%; max-width: 720px; max-height: 100%; display: flex;
           flex-direction: column; overflow: hidden; box-shadow: var(--lift); }
  .sheet header { display: flex; align-items: flex-start; gap: 12px; padding: 18px 16px 14px 24px; border-bottom: 1px solid var(--line); }
  .sheet header div { flex: 1; min-width: 0; }
  .sheet header strong { display: block; font-size: 16px; line-height: 1.35; overflow-wrap: anywhere; }
  .sheet header span { font-size: 12.5px; color: var(--muted); }
  .sheet .close { width: 38px; height: 38px; border-radius: 50%; background: var(--theirs); display: grid; place-items: center; flex: none; }
  .reading { overflow-y: auto; padding: 6px 24px 30px; font-size: 16.5px; line-height: 1.65; overflow-wrap: anywhere; }
  .reading h1 { font-size: 25px; line-height: 1.2; margin: 20px 0 12px; }
  .reading h2 { font-size: 18px; color: var(--text); margin: 28px 0 6px; letter-spacing: -0.01em; }
  .reading h3 { font-size: 16px; margin: 20px 0 4px; }
  .reading p, .reading ul, .reading ol { margin: 10px 0; }
  .reading ul, .reading ol { padding-left: 22px; }
  .reading li { margin: 5px 0; }
  .reading a { color: var(--brand); text-underline-offset: 3px; }
  .reading hr { border: 0; border-top: 1px solid var(--line); margin: 24px 0; }
  .scroll { overflow-x: auto; margin: 14px 0; border: 1px solid var(--line); border-radius: 14px; }
  table { border-collapse: collapse; font-size: 14.5px; line-height: 1.4; width: 100%; }
  th, td { padding: 9px 14px; text-align: left; vertical-align: top; overflow-wrap: normal; min-width: 5.5em; border-top: 1px solid var(--line); }
  th { font-weight: 600; background: var(--canvas); border-top: 0; }

  /* phones: tabs move to the bottom */
  .dock { display: none; }
  @media (max-width: 700px) {
    .bar { padding: 12px 16px; }
    .bar .tabs { display: none; }
    main { padding: 4px 14px 16px; }
    .dock { display: flex; position: fixed; left: 0; right: 0; bottom: 0; z-index: 4; background: var(--card);
            border-top: 1px solid var(--line); padding: 6px 8px calc(6px + env(safe-area-inset-bottom)); }
    .dock button { flex: 1; display: flex; flex-direction: column; align-items: center; gap: 2px; padding: 6px 4px;
                   font-size: 11.5px; font-weight: 600; color: var(--muted); border-radius: 12px; }
    .dock button[aria-current="page"] { color: var(--brand); }
    .app { padding-bottom: calc(58px + env(safe-area-inset-bottom)); }
    .compose { bottom: calc(58px + env(safe-area-inset-bottom)); padding: 6px 14px 10px; }
    .bubble { max-width: 92%; }
    .shade { padding: 0; align-items: stretch; }
    .sheet { border-radius: 0; max-width: none; }
    .reading { padding: 4px 18px 30px; }
    .sheet header { padding-left: 18px; }
  }
</style>
</head>
<body>
<svg hidden>
  <symbol id="i-chat" viewBox="0 0 24 24"><path d="M20 11.5a7.5 7.5 0 0 1-11 6.6L4.5 19.5l1.4-4.2A7.5 7.5 0 1 1 20 11.5Z"/></symbol>
  <symbol id="i-idea" viewBox="0 0 24 24"><path d="M9 17.5h6M10 20.5h4M8.2 14.2a6 6 0 1 1 7.6 0c-.6.5-.8 1.1-.8 1.8H9c0-.7-.2-1.3-.8-1.8Z"/></symbol>
  <symbol id="i-memory" viewBox="0 0 24 24"><path d="M7 4.5h10a1 1 0 0 1 1 1v14l-6-3.6-6 3.6v-14a1 1 0 0 1 1-1Z"/></symbol>
  <symbol id="i-send" viewBox="0 0 24 24"><path d="M12 19V5.5M6 11l6-6 6 6"/></symbol>
  <symbol id="i-search" viewBox="0 0 24 24"><circle cx="11" cy="11" r="6"/><path d="m20 20-4.5-4.5"/></symbol>
  <symbol id="i-doc" viewBox="0 0 24 24"><path d="M7 3.5h7l4 4v12a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1v-15a1 1 0 0 1 1-1ZM14 3.5v4h4M9 12.5h6M9 16h6"/></symbol>
  <symbol id="i-mic" viewBox="0 0 24 24"><rect x="9" y="3.5" width="6" height="11" rx="3"/><path d="M5.5 11.5a6.5 6.5 0 0 0 13 0M12 18v2.5"/></symbol>
  <symbol id="i-next" viewBox="0 0 24 24"><path d="m9.5 6 6 6-6 6"/></symbol>
  <symbol id="i-x" viewBox="0 0 24 24"><path d="M6.5 6.5l11 11M17.5 6.5l-11 11"/></symbol>
  <symbol id="i-again" viewBox="0 0 24 24"><path d="M5 12a7 7 0 1 1 2.2 5.1M5 18v-4h4"/></symbol>
</svg>
<div class="app">
  <header class="bar">
    <div class="face"><b id="mono">m</b><i id="presence"></i></div>
    <div class="who"><strong>Muse</strong><span id="how" aria-live="polite"></span></div>
    <nav class="tabs" aria-label="Sections"></nav>
  </header>
  <main id="view"></main>
  <div class="compose" id="compose" hidden>
    <form class="box" id="askform">
      <textarea id="task" rows="1" aria-label="Message Muse" placeholder="Ask Muse to look into something"></textarea>
      <button class="send" id="askbtn" aria-label="Send"><svg><use href="#i-send"/></svg></button>
    </form>
    <p class="hint" id="asknote" aria-live="polite"></p>
  </div>
  <nav class="dock tabs-dock" aria-label="Sections"></nav>
</div>
<div class="shade" id="shade" hidden><div class="sheet" role="dialog" aria-modal="true" aria-labelledby="sheettitle" id="sheet"></div></div>
<script>
"use strict";
const IDEAS = [
  "Which budget 3D printer should a beginner buy, and why?",
  "Compare three good mechanical keyboards under $150 for programming.",
  "Plan a relaxed two-day weekend trip near where I live, with good food stops.",
  "Summarise this week's news about AI agents, with links.",
  "Find three well-reviewed dinners I can cook in 30 minutes with chicken and rice.",
  "What are the best ways to make a small ESP32 gadget last longer on its battery?",
];
const SAY = [
  ["Set a timer for five minutes for tea", "Says so when it is up, and waves"],
  ["Remember that I like green tea", "Keeps it, and uses it later"],
  ["How's your battery?", "Reads its own sensors"],
  ["Dance for me", "Dances, with music"],
  ["Come closer", "Comes right up to the glass"],
  ["What did you find?", "Tells you about its latest research"],
  ["Good night", "Closes its eyes and sleeps"],
];
const TABS = [["activity", "Chat", "i-chat"], ["ideas", "Ideas", "i-idea"], ["memory", "Memory", "i-memory"]];
const icon = name => `<svg aria-hidden="true"><use href="#${name}"/></svg>`;

let token = "";
try {
  if (location.hash.length > 1 && !location.hash.startsWith("#/")) {
    localStorage.setItem("muse_token", decodeURIComponent(location.hash.slice(1)));
    history.replaceState(null, "", location.pathname);
  }
  token = localStorage.getItem("muse_token") || "";
} catch (e) {}

const $ = id => document.getElementById(id);
const view = $("view"), compose = $("compose"), task = $("task"), shade = $("shade"), sheet = $("sheet");
let jobs = [], app = null, canRead = true, drawn = "", lastCount = -1, notice = "", reading = "";

// Your own picture for Muse, if the server has one (companion/avatar.png).
const picture = new Image();
picture.alt = "";
picture.onload = () => $("mono").replaceWith(picture);
picture.src = "/avatar.png";

for (const nav of document.querySelectorAll(".tabs, .dock"))
  nav.innerHTML = TABS.map(([go, name, sym]) => `<button data-go="${go}">${icon(sym)}<span>${name}</span></button>`).join("");

const esc = s => String(s).replace(/[&<>"']/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[c]));
const sentence = s => { s = String(s || "").trim(); return s ? s.charAt(0).toUpperCase() + s.slice(1) + (/[.!?]$/.test(s) ? "" : ".") : ""; };

function inline(s) {
  s = esc(s);
  s = s.replace(/`([^`]+)`/g, "<code>$1</code>");
  s = s.replace(/\[([^\]]+)\]\((https?:\/\/[^\s)]+)\)/g, '<a href="$2" target="_blank" rel="noopener noreferrer">$1</a>');
  s = s.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
  s = s.replace(/(^|[\s(])\*([^*\s][^*]*)\*/g, "$1<em>$2</em>");
  return s;
}

// A small markdown renderer: headings, lists, tables, rules and paragraphs.
function markdown(text) {
  const lines = String(text).replace(/\r/g, "").split("\n");
  let html = "", i = 0;
  const cells = line => line.trim().replace(/^\||\|$/g, "").split("|").map(c => c.trim());
  while (i < lines.length) {
    const line = lines[i];
    let m;
    if (!line.trim()) { i++; continue; }
    if ((m = line.match(/^(#{1,3})\s+(.*)$/))) { html += `<h${m[1].length}>${inline(m[2])}</h${m[1].length}>`; i++; continue; }
    if (/^\s*(-{3,}|\*{3,})\s*$/.test(line)) { html += "<hr>"; i++; continue; }
    if (line.trim().startsWith("|") && i + 1 < lines.length && /^\s*\|?[\s:|-]+\|[\s:|-]*$/.test(lines[i + 1])) {
      html += '<div class="scroll"><table><thead><tr>' + cells(line).map(c => `<th>${inline(c)}</th>`).join("") + "</tr></thead><tbody>";
      i += 2;
      while (i < lines.length && lines[i].trim().startsWith("|")) {
        html += "<tr>" + cells(lines[i]).map(c => `<td>${inline(c)}</td>`).join("") + "</tr>";
        i++;
      }
      html += "</tbody></table></div>";
      continue;
    }
    if ((m = line.match(/^\s*([-*]|\d+[.)])\s+/))) {
      const tag = /\d/.test(m[1]) ? "ol" : "ul";
      html += `<${tag}>`;
      while (i < lines.length && (m = lines[i].match(/^\s*([-*]|\d+[.)])\s+(.*)$/))) { html += `<li>${inline(m[2])}</li>`; i++; }
      html += `</${tag}>`;
      continue;
    }
    const para = [];
    while (i < lines.length && lines[i].trim() && !/^(#{1,3}\s|\s*([-*]|\d+[.)])\s|\s*\|)/.test(lines[i])) { para.push(lines[i].trim()); i++; }
    if (!para.length) { para.push(lines[i].trim()); i++; }
    html += `<p>${inline(para.join(" "))}</p>`;
  }
  return html;
}

async function call(path, body) {
  const headers = token ? {Authorization: "Bearer " + token} : {};
  const init = {headers, cache: "no-store"};
  if (body) { init.method = "POST"; init.body = JSON.stringify(body); headers["Content-Type"] = "application/json"; headers["X-Muse"] = "1"; }
  const r = await fetch(path, init);
  if (r.status === 401 || r.status === 403) throw new Error("signin");
  if (!r.ok) throw new Error("The server answered " + r.status + ".");
  return r.json();
}

const route = () => (location.hash.startsWith("#/") ? location.hash.slice(2) : "activity").split("/");
const go = where => { location.hash = "#/" + where; };

const timeOf = s => new Date(s * 1000).toLocaleTimeString(undefined, {hour: "numeric", minute: "2-digit"});
function dayOf(seconds) {
  const d = new Date(seconds * 1000), now = new Date();
  const days = Math.round((new Date(now.getFullYear(), now.getMonth(), now.getDate()) - new Date(d.getFullYear(), d.getMonth(), d.getDate())) / 864e5);
  return days === 0 ? "Today" : days === 1 ? "Yesterday" : d.toLocaleDateString(undefined, {weekday: "long", day: "numeric", month: "long"});
}
const minutes = s => s < 90 ? "about a minute" : Math.round(s / 60) + " minutes";

function signIn(why) {
  return `<div class="signin"><h1>Sign in</h1><p class="lede">${why}</p>
    <p>On your computer, <code>python3 companion/muse_companion.py results</code> opens this page signed in.
    Or paste the token from <code>~/.config/muse/server.json</code>.</p>
    <form class="addrow" id="signform"><input class="field" type="password" id="tok" aria-label="Token" autocomplete="off" placeholder="Token">
    <button class="solid">Sign in</button></form><p class="status bad" id="signnote" aria-live="polite"></p></div>`;
}

function jobHtml(j) {
  let reply;
  if (j.status === "running") {
    reply = `<div class="bubble">On it. I'll say when it's ready.</div>
      <p class="doing">${icon("i-search")}Searching the web and reading</p>
      <div class="bubble dots" role="status" aria-label="Muse is working"><i></i><i></i><i></i></div>`;
  } else if (j.status === "done") {
    reply = `<div class="bubble">${esc(j.summary || "The write-up is ready.")}</div>
      <div class="card"><div class="head"><div class="tile">${icon("i-doc")}</div>
        <div><strong>${esc(j.title)}</strong><span>${j.words ? j.words + " words, " : ""}${j.took_s ? minutes(j.took_s) + " of work" : "write-up"}</span></div></div>
        <button class="wide" data-read="${esc(j.id)}">${icon("i-doc")}Read the write-up</button></div>`;
  } else {
    reply = `<div class="bubble bad">I couldn't finish this one. ${esc(sentence(j.error))}</div>
      <button class="chip" data-again="${esc(j.id)}">${icon("i-again")}Ask again</button>`;
  }
  return `<div class="msg me"><div class="bubble">${esc(j.task || j.title)}</div><span class="time">${timeOf(j.created)}</span></div>
    <div class="msg muse">${reply}</div>`;
}

function drawActivity() {
  if (!canRead) return signIn("Your conversation with Muse is private on this server.");
  if (!jobs.length) {
    return `<div class="welcome"><div class="tile">${icon("i-search")}</div><h1>Hand Muse something bigger</h1>
      <p>Ask out loud or type it here. Muse looks into it for a few minutes, tells you when it's ready, and leaves the write-up in this chat.</p>
      <div class="starters">${IDEAS.slice(0, 3).map(t => `<button data-idea="${esc(t)}">${esc(t)}</button>`).join("")}</div></div>`;
  }
  let html = "", day = "";
  for (const j of jobs.slice().reverse()) {
    const d = dayOf(j.created);
    if (d !== day) { html += `<p class="day"><span>${d}</span></p>`; day = d; }
    html += jobHtml(j);
  }
  return html;
}

function drawIdeas() {
  return `<h1>Ideas</h1><p class="lede">Tap one to start from it. You can change the words before you send.</p>
    <h2>Hand it to Muse</h2>
    <div class="group">${IDEAS.map(t => `<button class="item" data-idea="${esc(t)}"><span class="tile">${icon("i-search")}</span>
      <span class="words"><b>${esc(t)}</b></span>${icon("i-next")}</button>`).join("")}</div>
    <h2>Say it out loud: hold BOOT on Muse, speak, let go</h2>
    <div class="group">${SAY.map(([q, what]) => `<div class="item"><span class="tile">${icon("i-mic")}</span>
      <span class="words"><b>${esc(q)}</b><span>${esc(what)}</span></span></div>`).join("")}</div>`;
}

function drawMemory() {
  if (!app) return signIn("What Muse remembers about you is private.");
  const notes = app.memory || [];
  return `<h1>Memory</h1><p class="lede">What Muse remembers about you. It uses these when you talk, and hands them to its research.</p>
    <form class="addrow" id="addform"><input class="field" id="note" aria-label="Something for Muse to remember" maxlength="160"
      placeholder="Something for Muse to remember"><button class="solid">Remember</button></form>
    <p class="status" id="memnote" aria-live="polite">${notice}</p>
    <div class="group">${notes.length ? notes.map(n => `<div class="item"><span class="tile">${icon("i-memory")}</span>
      <span class="words">${esc(n)}</span><button class="round" data-forget="${esc(n)}" aria-label="Forget: ${esc(n)}">${icon("i-x")}</button></div>`).join("")
      : `<p class="empty">Nothing yet. Tell Muse out loud ("remember that I like green tea") or add a note above.</p>`}</div>`;
}

async function openSheet(id) {
  if (reading === id) return;
  reading = id;
  let j;
  try { j = await call("/results/" + encodeURIComponent(id) + ".json"); }
  catch (e) { j = {title: "Could not open that one", status: "failed", error: e.message === "signin" ? "sign in first" : e.message, created: Date.now() / 1000}; }
  const body = j.status === "done" ? markdown(j.report || "")
    : j.status === "running" ? `<p>Muse is still working on this.</p>` : `<p>${esc(sentence(j.error))}</p>`;
  sheet.innerHTML = `<header><div><strong id="sheettitle">${esc(j.task || j.title)}</strong>
    <span>${dayOf(j.created)}, ${timeOf(j.created)}</span></div>
    <button class="close" data-go="activity" aria-label="Close">${icon("i-x")}</button></header><div class="reading" tabindex="0">${body}</div>`;
  shade.hidden = false;
  document.body.style.overflow = "hidden";
  sheet.querySelector(".close").focus();
}
function closeSheet() { reading = ""; shade.hidden = true; document.body.style.overflow = ""; }

function drawBar() {
  const tab = route()[0] === "read" ? "activity" : route()[0];
  for (const b of document.querySelectorAll("[data-go]")) if (b.closest(".tabs, .dock")) b.setAttribute("aria-current", b.dataset.go === tab ? "page" : "false");
  let how = canRead ? "Sign in to ask and see how Muse is" : "";
  if (app) {
    const bits = [app.connected ? "Connected" : "Not connected"];
    if (app.muse && app.muse.battery_percent != null) bits.push("battery " + app.muse.battery_percent + "%" + (app.muse.power === "charging" ? ", charging" : ""));
    for (const t of app.timers || []) bits.push(t.label + " in " + (t.minutes_left < 1 ? "under a minute" : Math.round(t.minutes_left) + " min"));
    how = bits.join(", ");
  }
  $("how").textContent = how;
  $("presence").className = app && app.connected ? "on" : "";
}

async function draw() {
  const [where, id] = route();
  drawBar();
  if (where === "read" && id) await openSheet(id); else closeSheet();
  const tab = where === "ideas" || where === "memory" ? where : "activity";
  let html, key;
  if (tab === "ideas") { key = "ideas"; html = drawIdeas(); }
  else if (tab === "memory") { key = "memory" + JSON.stringify(app && app.memory) + notice; html = drawMemory(); }
  else { key = "activity" + canRead + JSON.stringify(jobs.map(j => [j.id, j.status])); html = drawActivity(); }
  const asking = tab === "activity" && canRead;
  compose.hidden = !asking;
  if (asking) {
    const why = app ? app.cannot_work : "";
    $("askbtn").disabled = !app || Boolean(why);
    task.disabled = !app;
    $("asknote").innerHTML = !app ? `<button class="link" data-go="memory">Sign in</button> to ask from here. You can always ask Muse out loud.` : esc(sentence(why));
  }
  if (key === drawn) return;
  const fresh = !drawn.startsWith(tab);
  drawn = key;
  view.innerHTML = html;
  if (tab === "activity" && (fresh || jobs.length !== lastCount)) window.scrollTo(0, document.documentElement.scrollHeight);
  else if (fresh) window.scrollTo(0, 0);
  lastCount = jobs.length;
}

async function refresh() {
  try { jobs = (await call("/results.json")).jobs || []; canRead = true; }
  catch (e) { if (e.message === "signin") { canRead = false; jobs = []; } }
  try { app = await call("/app.json"); }
  catch (e) { if (e.message === "signin") app = null; }
  await draw();
}

async function ask(text) {
  text = text.trim();
  if (!text || !app) return;
  $("askbtn").disabled = true;
  try {
    const r = await call("/ask", {task: text});
    if (r.ok) { task.value = ""; grow(); go("activity"); }
    else $("asknote").textContent = sentence(r.error || "Muse could not start that");
  } catch (e) { $("asknote").textContent = e.message === "signin" ? "Sign in to ask from here." : e.message; }
  await refresh();
}

function grow() { task.style.height = "auto"; task.style.height = Math.min(task.scrollHeight, 140) + "px"; }

document.addEventListener("click", async e => {
  if (e.target === shade) return go("activity");
  const b = e.target.closest("button");
  if (!b) return;
  if (b.dataset.go) go(b.dataset.go);
  else if (b.dataset.read) go("read/" + b.dataset.read);
  else if (b.dataset.idea) { go("activity"); task.value = b.dataset.idea; setTimeout(() => { grow(); task.focus(); }, 0); }
  else if (b.dataset.again) { const j = jobs.find(x => x.id === b.dataset.again); if (j) ask(j.task); }
  else if (b.dataset.forget) {
    const text = b.dataset.forget;
    await call("/memory", {forget: text}).catch(() => {});
    notice = `Forgotten. <button class="link" data-undo="${esc(text)}">Undo</button>`;
    await refresh();
  } else if (b.dataset.undo) { await call("/memory", {add: b.dataset.undo}).catch(() => {}); notice = ""; await refresh(); }
});

document.addEventListener("submit", async e => {
  e.preventDefault();
  if (e.target.id === "askform") ask(task.value);
  else if (e.target.id === "addform") {
    const text = $("note").value.trim();
    if (!text) return;
    const r = await call("/memory", {add: text}).catch(err => ({error: err.message}));
    notice = r.ok ? "" : esc(sentence(r.error || "Could not save that"));
    await refresh();
  } else if (e.target.id === "signform") {
    token = $("tok").value.trim();
    try { await call("/app.json"); try { localStorage.setItem("muse_token", token); } catch (err) {} drawn = ""; await refresh(); }
    catch (err) { token = ""; $("signnote").textContent = err.message === "signin" ? "That token was not accepted." : err.message; }
  }
});

document.addEventListener("keydown", e => { if (e.key === "Escape" && !shade.hidden) go("activity"); });
task.addEventListener("input", grow);
task.addEventListener("keydown", e => { if (e.key === "Enter" && !e.shiftKey && !e.isComposing) { e.preventDefault(); ask(task.value); } });
window.addEventListener("hashchange", () => { notice = ""; draw(); });

refresh();
setInterval(() => {
  if (document.hidden || document.activeElement === $("tok") || document.activeElement === $("note")) return;
  refresh();
}, 7000);
</script>
</body>
</html>
"""
