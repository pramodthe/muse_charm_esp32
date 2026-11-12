# SPDX-License-Identifier: MIT
"""Muse's web app: a conversation with Muse about the bigger things you hand it.

    GET  /results             the app (one page; it holds no data itself)
    GET  /results.json        the jobs, newest first, without their write-ups
    GET  /results/<id>.json   one job with its write-up
    GET  /feed.json           the conversation, the jobs and Muse's state in one
    GET  /app.json            Muse's state, timers and memory
    POST /chat                {"text": "..."} a typed message; Muse answers (muse_chat.py);
                              {"remove": [t, ...]} takes entries out of the conversation
    POST /ask                 {"task": "..."} starts a background job directly
    POST /memory              {"add": "..."} or {"forget": "..."}
    POST /reminders           {"cancel": "<id>"}
    GET  /avatar              your own picture for Muse, if there is a companion/avatar.webp
                              (or .gif, .png, .jpg); it is the browser tab's icon too

Online, everything needs the server's token, except that MUSE_RESULTS_PUBLIC
"true" lets anyone who has the address read the jobs (the two /results
addresses). The page keeps the token in the browser: `muse_companion.py
results` opens the page with the token after the "#" of the address (that part
is never sent to a server), and the page stores it and removes it from the
address bar. The token is MUSE_TOKEN in the repository's .env file. On your
own computer, without a token, the page is open and only this computer may
post.

It is laid out as a chat, the way OpenMuse (https://github.com/CopilotKit/openmuse,
MIT) is: Muse at the top, your requests on the right, Muse's answers on the
left with a card for each write-up, a message box and the tabs at the bottom.
The colours, corner radii and type sizes follow the Meta Horizon OS UI Set
(its public Figma design kit); the icons are this project's own.
"""

PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="robots" content="noindex">
<meta name="color-scheme" content="light">
<meta name="theme-color" content="#fcfcfc">
<title>Muse</title>
<link rel="icon" id="favicon" href="data:,">
<style>
  /* Light only. Colours, radii and type sizes follow the Meta Horizon OS UI Set (text #272727, surfaces at
     5 % of it, #f2f2f2 bubbles, pill buttons, 14/20 and 12/16 medium text); the pale blue of your own
     messages and the overall chat layout follow OpenMuse. */
  :root {
    color-scheme: light;
    --canvas: #fcfcfc; --card: #ffffff; --text: #272727; --muted: rgba(39, 39, 39, 0.7); --faint: rgba(39, 39, 39, 0.45);
    --line: #eeeef0; --fill: rgba(39, 39, 39, 0.05); --fill-2: rgba(39, 39, 39, 0.1); --theirs: #f2f2f2; --mine: #c8e7ff;
    --ink: #272727; --ink-label: #ffffff; --link: #1473c8; --ok: #1f9d55; --danger: #c22a3c; --danger-bg: #fbefed;
    --shade: rgba(39, 39, 39, 0.32);
    --punched: inset 0 1px 1px rgba(39, 39, 39, 0.05), 0 1px 1px #ffffff;
    --lift: 0 1px 2px rgba(39, 39, 39, 0.06), 0 10px 30px rgba(39, 39, 39, 0.08);
  }
  * { box-sizing: border-box; }
  html { -webkit-text-size-adjust: 100%; }
  body { margin: 0; background: var(--canvas); color: var(--text);
         font: 15px/22px Inter, system-ui, -apple-system, "Segoe UI", Roboto, "Helvetica Neue", sans-serif; }
  button, input, textarea { font: inherit; color: inherit; }
  button { cursor: pointer; border: 0; background: none; padding: 0; }
  svg { width: 20px; height: 20px; fill: none; stroke: currentColor; stroke-width: 1.8; stroke-linecap: round;
        stroke-linejoin: round; flex: none; }
  :focus-visible { outline: 2px solid var(--link); outline-offset: 2px; }
  [hidden] { display: none !important; }
  .app { max-width: 720px; margin: 0 auto; min-height: 100vh; min-height: 100dvh; display: flex; flex-direction: column;
         padding-bottom: calc(76px + env(safe-area-inset-bottom)); }

  /* top: who you are talking to */
  .bar { position: sticky; top: 0; z-index: 3; display: flex; flex-direction: column; align-items: center; gap: 2px;
         padding: 14px 20px 8px; background: var(--canvas); }
  .bar::after { content: ""; position: absolute; left: 0; right: 0; top: 100%; height: 16px; pointer-events: none;
                background: linear-gradient(to bottom, var(--canvas), transparent); }
  .face { position: relative; width: 64px; height: 64px; }
  .face b { width: 64px; height: 64px; border-radius: 50%; display: grid; place-items: center;
            background: var(--mine); color: var(--text); font-size: 26px; font-weight: 600; }
  /* Your picture of Muse sits on black, the way Muse does on its own screen, in a thin pale ring */
  .face img { display: block; width: 64px; height: 64px; border-radius: 50%; object-fit: cover; background: #000;
              box-shadow: 0 0 0 3px var(--canvas), 0 0 0 4.5px var(--mine), 0 6px 16px rgba(39, 39, 39, 0.18); }
  .face i { position: absolute; right: 0; bottom: 2px; width: 15px; height: 15px; border-radius: 50%;
            background: #b8b8b8; border: 2.5px solid var(--canvas); }
  .face i.on { background: var(--ok); }
  .who { text-align: center; }
  .who strong { display: block; font-size: 16px; line-height: 22px; font-weight: 600; margin-top: 2px; }
  .who span { display: inline-flex; align-items: center; gap: 6px; max-width: 100%; margin-top: 4px; padding: 4px 12px;
              border-radius: 1000px; background: var(--fill); font-size: 12px; line-height: 16px; font-weight: 500; color: var(--muted); }
  .who span:empty { display: none; }

  main { flex: 1; padding: 4px 20px 16px; min-width: 0; }
  h1 { font-size: 22px; line-height: 28px; font-weight: 600; letter-spacing: -0.02em; margin: 10px 0 4px; }
  h2 { font-size: 12px; line-height: 16px; color: var(--muted); font-weight: 500; margin: 24px 12px 8px; }
  .lede { color: var(--muted); margin: 0 0 6px; max-width: 54ch; font-size: 14px; line-height: 20px; }

  /* the conversation */
  .day { text-align: center; margin: 20px 0 12px; font-size: 12px; line-height: 16px; font-weight: 500; color: var(--faint); }
  .msg { display: flex; flex-direction: column; margin: 6px 0; }
  .msg.me { align-items: flex-end; margin-top: 16px; }
  .msg.muse { align-items: flex-start; gap: 6px; }
  .bubble { padding: 10px 16px; border-radius: 22px; overflow-wrap: anywhere; max-width: 85%; }
  .me .bubble { background: var(--mine); }
  .muse .bubble { background: var(--theirs); max-width: 95%; }
  .bubble.bad { background: var(--danger-bg); color: var(--danger); }
  .bubble[data-t] { cursor: pointer; }
  .bubble.picked { box-shadow: 0 0 0 2px var(--text); }
  .remove { font-size: 12px; line-height: 16px; font-weight: 600; color: var(--danger); margin: 4px 8px 0; padding: 4px 6px; border-radius: 8px; }
  .remove:hover { background: var(--danger-bg); }
  .time { font-size: 11px; line-height: 16px; color: var(--faint); margin: 3px 8px 0; }
  .doing { display: flex; align-items: center; gap: 8px; color: var(--muted); font-size: 14px; line-height: 20px; margin: 6px 12px; }
  .doing svg { width: 17px; height: 17px; flex: none; }
  .looked { font-size: 12px; line-height: 16px; margin: 2px 12px; }
  .looked a { color: var(--link); text-decoration: none; }
  .looked a:hover { text-decoration: underline; }
  .dots { display: inline-flex; gap: 4px; padding: 15px 16px; }
  .dots i { width: 7px; height: 7px; border-radius: 50%; background: var(--faint); animation: hop 1.3s ease-in-out infinite; }
  .dots i:nth-child(2) { animation-delay: 0.16s; } .dots i:nth-child(3) { animation-delay: 0.32s; }
  @keyframes hop { 0%, 60%, 100% { transform: translateY(0); opacity: 0.5; } 30% { transform: translateY(-5px); opacity: 1; } }
  @media (prefers-reduced-motion: reduce) { .dots i { animation: none; } }
  .card { background: var(--theirs); border-radius: 24px; padding: 18px; width: min(100%, 420px); }
  .card .head { display: flex; gap: 12px; align-items: center; }
  .tile { width: 40px; height: 40px; border-radius: 12px; background: var(--card); color: var(--link); display: grid;
          place-items: center; flex: none; }
  .card .head strong { display: block; font-weight: 600; line-height: 20px; overflow-wrap: anywhere; }
  .card .head span { font-size: 12px; line-height: 16px; color: var(--muted); }
  .wide { display: flex; align-items: center; justify-content: center; gap: 8px; width: 100%; margin-top: 14px; padding: 10px;
          border-radius: 1000px; background: var(--card); font-weight: 600; font-size: 14px; line-height: 20px; }
  .wide:hover { background: #fafafa; box-shadow: 0 0 0 1px var(--line); }
  .chip { display: inline-flex; align-items: center; gap: 6px; padding: 8px 14px; border-radius: 1000px;
          background: var(--fill); font-weight: 500; font-size: 14px; line-height: 20px; }
  .chip:hover { background: var(--fill-2); }
  .chip svg { width: 17px; height: 17px; }
  .welcome { text-align: center; padding: 6vh 10px 20px; }
  .welcome h1 { margin: 0 0 6px; }
  .welcome p { color: var(--muted); max-width: 40ch; margin: 0 auto 18px; font-size: 14px; line-height: 21px; }
  .starters { display: flex; flex-direction: column; gap: 8px; max-width: 380px; margin: 0 auto; }
  .starters button { background: var(--fill); border-radius: 16px; padding: 11px 16px; text-align: left; font-size: 14px; line-height: 20px; font-weight: 500; }
  .starters button:hover { background: var(--fill-2); }
  .link { color: var(--link); font-weight: 600; }

  /* the message box, and the tabs under it */
  .compose { position: sticky; bottom: calc(68px + env(safe-area-inset-bottom)); z-index: 2; padding: 6px 20px 8px;
             background: linear-gradient(to bottom, transparent, var(--canvas) 30%); }
  .box { display: flex; align-items: flex-end; gap: 8px; background: var(--card); border: 1px solid var(--line);
         border-radius: 32px; padding: 7px 7px 7px 20px; box-shadow: var(--lift); }
  .box textarea { flex: 1; min-width: 0; border: 0; outline: 0; background: none; resize: none; padding: 9px 0; line-height: 22px;
                  max-height: 136px; }
  .box textarea::placeholder { color: var(--faint); }
  .send { width: 40px; height: 40px; border-radius: 50%; background: var(--ink); color: var(--ink-label); display: grid;
          place-items: center; flex: none; }
  .send:disabled { background: var(--fill); color: var(--faint); cursor: default; }
  .compose .hint { font-size: 12px; line-height: 16px; color: var(--muted); margin: 8px 12px 0; text-align: center; min-height: 0; }
  .compose .hint:empty { display: none; }
  .dock { position: fixed; left: 50%; bottom: calc(14px + env(safe-area-inset-bottom)); transform: translateX(-50%); z-index: 4;
          display: flex; gap: 4px; padding: 6px; border-radius: 1000px; background: var(--card); border: 1px solid var(--line);
          box-shadow: var(--lift); }
  .dock button { display: flex; align-items: center; gap: 8px; padding: 9px 16px; border-radius: 1000px; font-size: 13px;
                 line-height: 16px; font-weight: 500; color: var(--muted); }
  .dock button svg { width: 18px; height: 18px; }
  .dock button:hover { background: var(--fill); }
  .dock button[aria-current="page"] { background: var(--ink); color: var(--ink-label); }

  /* lists: ideas and memory */
  .group { background: var(--fill); border-radius: 16px; overflow: hidden; box-shadow: var(--punched); }
  .item { display: flex; align-items: center; gap: 12px; width: 100%; min-height: 60px; padding: 12px 14px; text-align: left; }
  .item + .item { border-top: 1px solid var(--line); }
  button.item:hover { background: var(--fill); }
  .item .words { flex: 1; min-width: 0; overflow-wrap: anywhere; font-size: 14px; line-height: 20px; font-weight: 500; }
  .item .words b { display: block; font-weight: 500; }
  .item .words span { color: var(--muted); font-size: 12px; line-height: 16px; }
  .item > svg { color: var(--faint); width: 18px; height: 18px; }
  .item .tile { width: 36px; height: 36px; border-radius: 50%; color: var(--text); }
  .round { width: 36px; height: 36px; border-radius: 50%; display: grid; place-items: center; color: var(--muted); flex: none; }
  .round:hover { background: var(--danger-bg); color: var(--danger); }
  .round svg { width: 17px; height: 17px; }
  .addrow { display: flex; gap: 8px; margin: 14px 0 6px; }
  .field { flex: 1; min-width: 0; height: 44px; border: 0; border-radius: 8px; background: var(--fill); padding: 0 16px;
           font-size: 14px; box-shadow: var(--punched); }
  .field::placeholder { color: var(--muted); }
  .field:focus { outline: 2px solid var(--ink); outline-offset: 0; }
  .solid { background: var(--ink); color: var(--ink-label); border-radius: 1000px; padding: 0 20px; height: 44px; font-weight: 500; font-size: 14px; }
  .solid:hover { background: #3d3d3d; }
  .status { font-size: 12px; line-height: 16px; color: var(--muted); margin: 6px 12px 10px; min-height: 16px; }
  .status.bad { color: var(--danger); }
  .empty { color: var(--muted); text-align: center; padding: 26px 20px; margin: 0; font-size: 14px; }
  .signin { background: var(--card); border: 1px solid var(--line); border-radius: 24px; padding: 24px; margin: 4vh auto 0;
            max-width: 440px; box-shadow: var(--lift); font-size: 14px; line-height: 21px; }
  .signin h1 { margin-top: 0; }
  code { font-family: ui-monospace, "SF Mono", Menlo, monospace; font-size: 0.86em; background: var(--fill); padding: 1px 5px; border-radius: 5px; }

  /* reading a write-up */
  .shade { position: fixed; inset: 0; z-index: 10; background: var(--shade); display: flex; align-items: center; justify-content: center; padding: 24px; }
  .sheet { background: var(--card); border-radius: 26px; width: 100%; max-width: 720px; max-height: 100%; display: flex;
           flex-direction: column; overflow: hidden; box-shadow: var(--lift); }
  .sheet header { display: flex; align-items: flex-start; gap: 12px; padding: 18px 16px 14px 24px; border-bottom: 1px solid var(--line); }
  .sheet header div { flex: 1; min-width: 0; }
  .sheet header strong { display: block; font-size: 15px; line-height: 21px; font-weight: 600; overflow-wrap: anywhere; }
  .sheet header span { font-size: 12px; line-height: 16px; color: var(--muted); }
  .sheet .close { width: 40px; height: 40px; border-radius: 50%; background: var(--fill); display: grid; place-items: center; flex: none; }
  .sheet .close:hover { background: var(--fill-2); }
  .reading { overflow-y: auto; padding: 6px 24px 30px; font-size: 16px; line-height: 26px; overflow-wrap: anywhere; }
  .reading h1 { font-size: 24px; line-height: 30px; margin: 20px 0 12px; }
  .reading h2 { font-size: 18px; line-height: 24px; color: var(--text); font-weight: 600; margin: 28px 0 6px; letter-spacing: -0.01em; }
  .reading h3 { font-size: 16px; font-weight: 600; margin: 20px 0 4px; }
  .reading p, .reading ul, .reading ol { margin: 10px 0; }
  .reading ul, .reading ol { padding-left: 22px; }
  .reading li { margin: 5px 0; }
  .reading a { color: var(--link); text-underline-offset: 3px; }
  .reading hr { border: 0; border-top: 1px solid var(--line); margin: 24px 0; }
  .scroll { overflow-x: auto; margin: 14px 0; border-radius: 16px; background: var(--fill); }
  table { border-collapse: collapse; font-size: 14px; line-height: 20px; width: 100%; }
  th, td { padding: 10px 14px; text-align: left; vertical-align: top; overflow-wrap: normal; min-width: 5.5em; border-top: 1px solid var(--line); }
  th { font-weight: 600; border-top: 0; }

  @media (max-width: 700px) {
    main { padding: 4px 14px 12px; }
    .compose { padding: 6px 14px 8px; }
    .dock button { padding: 9px 14px; }
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
  <symbol id="i-bell" viewBox="0 0 24 24"><path d="M6.5 16.5V11a5.5 5.5 0 0 1 11 0v5.5l1.5 2H5l1.5-2ZM10 20.5a2.2 2.2 0 0 0 4 0"/></symbol>
  <symbol id="i-again" viewBox="0 0 24 24"><path d="M5 12a7 7 0 1 1 2.2 5.1M5 18v-4h4"/></symbol>
</svg>
<div class="app">
  <header class="bar">
    <div class="face"><b id="mono">M</b><i id="presence"></i></div>
    <div class="who"><strong>Muse</strong><span id="how" aria-live="polite"></span></div>
  </header>
  <main id="view"></main>
  <div class="compose" id="compose" hidden>
    <form class="box" id="askform">
      <textarea id="task" rows="1" aria-label="Message Muse" placeholder="Message Muse"></textarea>
      <button class="send" id="askbtn" aria-label="Send"><svg><use href="#i-send"/></svg></button>
    </form>
    <p class="hint" id="asknote" aria-live="polite"></p>
  </div>
  <nav class="dock" aria-label="Sections"></nav>
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
  ["Will it rain this afternoon?", "Looks it up on the web, and tells you in a few seconds"],
  ["Set a timer for five minutes for tea", "Says so when it is up, and waves"],
  ["Remind me at five to call mum", "Says it out loud when it is time"],
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
let jobs = [], messages = [], app = null, canRead = true, drawn = "", lastCount = -1, notice = "", reading = "";
let pending = "";   // a typed message Muse is still answering
let picked = null;  // the time ("t") of the message that was tapped, to remove it
let stay = false;   // do not scroll on the next draw

// Your own picture for Muse, if the server has one (companion/avatar.webp, .gif, .png or .jpg).
// It is the tab's icon too (browsers show the first frame of an animated one).
const picture = new Image();
picture.alt = "";
picture.onload = () => { $("mono").replaceWith(picture); $("favicon").href = "/avatar"; };
picture.src = "/avatar";

document.querySelector(".dock").innerHTML = TABS.map(([go, name, sym]) => `<button data-go="${go}">${icon(sym)}<span>${name}</span></button>`).join("");

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
    <p>Paste your token: it is <code>MUSE_TOKEN</code> in the <code>.env</code> file of your Muse folder.
    You only do this once in each browser.</p>
    <form class="addrow" id="signform"><input class="field" type="password" id="tok" aria-label="Token" autocomplete="off" placeholder="Token">
    <button class="solid">Sign in</button></form><p class="status bad" id="signnote" aria-live="polite"></p></div>`;
}

// What Muse shows for a background job: working, the answer with its write-up, or that it failed.
function jobReply(j, onIt) {
  let reply;
  if (j.status === "running") {
    reply = `${onIt ? `<div class="bubble">On it. I'll say when it's ready.</div>` : ""}
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
  return `<div class="msg muse">${reply}</div>`;
}

// Muse looked something up on the web: where the answer came from, as links to the pages.
const lookedUp = e => {
  const links = (e.sources || []).filter(s => /^https?:\/\//.test(s.url))
    .map(s => `<a href="${esc(s.url)}" target="_blank" rel="noopener noreferrer" title="${esc(s.title)}">${esc(s.site || s.url)}</a>`);
  return `<p class="doing looked">${icon("i-search")}<span>Looked it up${links.length ? ": " + links.join(", ") : ""}</span></p>`;
};

// A message that is in the conversation can be picked (tap it) and then removed.
const pickable = t => app && t != null ? ` data-t="${t}" tabindex="0"` + (t === picked ? ' class="bubble picked"' : ' class="bubble"') : ' class="bubble"';
const remover = t => app && t != null && t === picked ? `<button class="remove" data-remove="${t}">Remove from the chat</button>` : "";
const mine = (text, t, aloud, kept) => `<div class="msg me"><div${pickable(kept ? t : null)}>${esc(text)}</div>
  <span class="time">${timeOf(t)}${aloud ? ", out loud" : ""}</span>${remover(kept ? t : null)}</div>`;

// Signed in: the whole conversation, spoken and typed, with each job where it was started.
// Otherwise (results made public): the jobs alone.
function entries() {
  if (!app) return jobs.map(j => [{t: j.created, who: "you", text: j.task || j.title}, {t: j.created + 0.001, job: j.id, onIt: true}]).flat();
  const shown = new Set(messages.filter(e => e.job).map(e => e.job));
  const older = jobs.filter(j => !shown.has(j.id)).map(j => [{t: j.created, who: "you", text: j.task || j.title}, {t: j.created + 0.001, job: j.id, onIt: true}]).flat();
  return messages.concat(older);
}

function drawActivity() {
  if (!canRead) return signIn("Your conversation with Muse is private on this server.");
  const list = entries().sort((a, b) => a.t - b.t);
  if (!list.length && !pending) {
    return `<div class="welcome"><h1>Say hello to Muse</h1>
      <p>Type here or talk to the Muse on your desk: it is one conversation. Ask for something bigger and Muse looks into it for a few minutes, then leaves the write-up in this chat.</p>
      <div class="starters">${IDEAS.slice(0, 3).map(t => `<button data-idea="${esc(t)}">${esc(t)}</button>`).join("")}</div></div>`;
  }
  const byId = Object.fromEntries(jobs.map(j => [j.id, j]));
  let html = "", day = "";
  for (const e of list) {
    const d = dayOf(e.t);
    if (d !== day) { html += `<p class="day">${d}</p>`; day = d; }
    if (e.job) html += byId[e.job] ? jobReply(byId[e.job], e.onIt) : "";
    else if (e.lookup) html += lookedUp(e);
    else if (e.who === "you") html += mine(e.text, e.t, e.via === "voice", e.via != null);
    else html += `<div class="msg muse"><div${pickable(e.t)}>${esc(e.text)}</div>${remover(e.t)}</div>`;
  }
  if (pending) html += mine(pending, Date.now() / 1000) +
    `<div class="msg muse"><div class="bubble dots" role="status" aria-label="Muse is typing"><i></i><i></i><i></i></div></div>`;
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
  const notes = app.memory || [], reminders = app.reminders || [];
  return `<h1>Memory</h1><p class="lede">What Muse is keeping for you: reminders it will say out loud, and what it remembers about you.</p>
    <h2>Reminders</h2>
    <div class="group">${reminders.length ? reminders.map(r => `<div class="item"><span class="tile">${icon("i-bell")}</span>
      <span class="words"><b>${esc(r.text)}</b><span>${esc(sentence(r.when))}</span></span>
      <button class="round" data-cancel="${esc(r.id)}" aria-label="Cancel the reminder: ${esc(r.text)}">${icon("i-x")}</button></div>`).join("")
      : `<p class="empty">No reminders. Ask Muse, out loud or in the chat: "remind me at 5 pm to call mum".</p>`}</div>
    <h2>What Muse remembers about you</h2>
    <p class="lede" style="margin:0 12px 4px">It uses these when you talk, and hands them to its research.</p>
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
  for (const b of document.querySelectorAll("[data-go]")) if (b.closest(".dock")) b.setAttribute("aria-current", b.dataset.go === tab ? "page" : "false");
  let how = canRead ? "Sign in to ask and see how Muse is" : "";
  if (app) {
    const bits = [app.connected ? "Connected" : "Not connected"];
    if (app.muse && app.muse.battery_percent != null) bits.push("battery " + app.muse.battery_percent + "%" + (app.muse.power === "charging" ? ", charging" : ""));
    const update = app.firmware && app.firmware.muse && app.firmware.muse.update;
    if (update && update.state === "downloading") bits.push("updating its firmware, " + (update.percent || 0) + "%");
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
  else if (tab === "memory") { key = "memory" + JSON.stringify(app && [app.memory, app.reminders]) + notice; html = drawMemory(); }
  else {
    const last = messages[messages.length - 1] || {};
    key = "activity" + canRead + Boolean(app) + pending + picked + messages.length + (last.text || "").length +
          JSON.stringify(jobs.map(j => [j.id, j.status]));
    html = drawActivity();
  }
  const asking = tab === "activity" && canRead;
  compose.hidden = !asking;
  if (asking) {
    $("askbtn").disabled = !app || Boolean(pending);
    task.disabled = !app;
    if (!app) $("asknote").innerHTML = `<button class="link" data-go="memory">Sign in</button> to write to Muse. You can always talk to it out loud.`;
    else if ($("asknote").querySelector("button")) $("asknote").textContent = "";
  }
  if (key === drawn) return;
  const fresh = !drawn.startsWith(tab);
  drawn = key;
  view.innerHTML = html;
  const count = jobs.length + messages.length + (pending ? 1 : 0);
  if (stay) stay = false;
  else if (tab === "activity" && (fresh || count !== lastCount)) window.scrollTo(0, document.documentElement.scrollHeight);
  else if (fresh) window.scrollTo(0, 0);
  lastCount = count;
}

async function refresh() {
  try {
    const feed = await call("/feed.json");            // signed in: everything in one answer
    app = feed.app; jobs = feed.jobs || []; messages = feed.messages || []; canRead = true;
  } catch (e) {
    if (e.message !== "signin") return draw();        // a network hiccup: keep what is shown
    app = null; messages = [];
    try { jobs = (await call("/results.json")).jobs || []; canRead = true; }   // results made public
    catch (err) { if (err.message === "signin") { canRead = false; jobs = []; } }
  }
  await draw();
}

// Send a typed message. Muse answers in a few seconds, and hands bigger things to a background job.
async function ask(text) {
  text = text.trim();
  if (!text || !app || pending) return;
  pending = text; task.value = ""; grow(); $("asknote").textContent = "";
  go("activity"); await draw();
  let problem = "";
  try {
    const r = await call("/chat", {text});
    if (!r.ok) problem = sentence(r.error || "Muse could not answer that");
  } catch (e) { problem = e.message === "signin" ? "Sign in to write to Muse." : e.message; }
  pending = "";
  await refresh();
  if (problem) { $("asknote").textContent = problem; if (!task.value) { task.value = text; grow(); } }
}

// Start a background job again, as it was.
async function again(text) {
  const r = await call("/ask", {task: text}).catch(e => ({error: e.message}));
  await refresh();
  if (!r.ok) $("asknote").textContent = sentence(r.error || "Muse could not start that");
}

function grow() { task.style.height = "auto"; task.style.height = Math.min(task.scrollHeight, 140) + "px"; }

document.addEventListener("click", async e => {
  if (e.target === shade) return go("activity");
  const b = e.target.closest("button");
  const bubble = !b && e.target.closest(".bubble[data-t]");
  if (bubble && !String(getSelection())) {    // tap a message to pick it; again to let go
    const t = Number(bubble.dataset.t);
    picked = picked === t ? null : t; stay = true;
    return draw();
  }
  if (!b) { if (picked != null && !e.target.closest(".bubble")) { picked = null; stay = true; draw(); } return; }
  if (b.dataset.remove) {
    await call("/chat", {remove: [Number(b.dataset.remove)]}).catch(() => {});
    picked = null; stay = true;
    return refresh();
  }
  if (b.dataset.go) go(b.dataset.go);
  else if (b.dataset.read) go("read/" + b.dataset.read);
  else if (b.dataset.idea) { go("activity"); task.value = b.dataset.idea; setTimeout(() => { grow(); task.focus(); }, 0); }
  else if (b.dataset.again) { const j = jobs.find(x => x.id === b.dataset.again); if (j) again(j.task); }
  else if (b.dataset.forget) {
    const text = b.dataset.forget;
    await call("/memory", {forget: text}).catch(() => {});
    notice = `Forgotten. <button class="link" data-undo="${esc(text)}">Undo</button>`;
    await refresh();
  } else if (b.dataset.cancel) {
    await call("/reminders", {cancel: b.dataset.cancel}).catch(() => {});
    notice = ""; await refresh();
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

document.addEventListener("keydown", e => {
  if (e.key === "Escape" && !shade.hidden) go("activity");
  else if (e.key === "Escape" && picked != null) { picked = null; stay = true; draw(); }
  else if (e.key === "Enter" && e.target.matches && e.target.matches(".bubble[data-t]")) e.target.click();
});
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
