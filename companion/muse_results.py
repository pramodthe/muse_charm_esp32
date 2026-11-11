# SPDX-License-Identifier: MIT
"""Muse's web app: read what Muse found, hand it something new, see how it is.

    GET  /results             the app (one page; it holds no data itself)
    GET  /results.json        the jobs, newest first, without their write-ups
    GET  /results/<id>.json   one job with its write-up
    GET  /app.json            Muse's state, timers and memory
    POST /ask                 {"task": "..."} starts a background job
    POST /memory              {"add": "..."} or {"forget": "..."}

Online, everything needs the server's token, except that MUSE_RESULTS_PUBLIC
"true" lets anyone who has the address read the jobs (the two /results
addresses). The page keeps the token in the browser: `muse_companion.py
results` opens the page with the token after the "#" of the address (that part
is never sent to a server), and the page stores it and removes it from the
address bar. On your own computer, without a token, the page is open and only
this computer may post.

Three views: Activity (each request, what Muse said, the write-up to read),
Ideas (things to hand over and things to say out loud) and Memory (what Muse
remembers about you). A request is shown as a highlighted line, the way you
would mark it on paper; Muse's answers are plain text beneath it.
"""

PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="robots" content="noindex">
<meta name="theme-color" content="#fbfbfa" media="(prefers-color-scheme: light)">
<meta name="theme-color" content="#171a1c" media="(prefers-color-scheme: dark)">
<title>Muse</title>
<style>
  :root {
    --paper: #fbfbfa; --panel: #f1f2ef; --ink: #1d2125; --soft: #5e666e; --line: #e1e3df;
    --mark: #ffe066; --mark-ink: #1d2125; --act: #0e6e6a; --act-ink: #ffffff; --bad: #b42318;
    --ui: ui-rounded, "SF Pro Rounded", "Nunito", "Varela Round", system-ui, -apple-system, "Segoe UI", sans-serif;
    --read: "Iowan Old Style", "Charter", "New York", ui-serif, Georgia, "Times New Roman", serif;
  }
  @media (prefers-color-scheme: dark) {
    :root { --paper: #171a1c; --panel: #1f2326; --ink: #eceeea; --soft: #9aa3a8; --line: #2c3236;
            --mark: #f2ce4a; --mark-ink: #1d2125; --act: #63cbc3; --act-ink: #10201f; --bad: #ff8a80; }
  }
  * { box-sizing: border-box; }
  html { -webkit-text-size-adjust: 100%; }
  body { margin: 0; background: var(--paper); color: var(--ink); font: 16px/1.5 var(--ui); }
  button, input, textarea { font: inherit; color: inherit; }
  button { cursor: pointer; }
  :focus-visible { outline: 2px solid var(--act); outline-offset: 2px; border-radius: 4px; }
  [hidden] { display: none !important; }

  /* frame: a rail and one reading column */
  .frame { display: grid; grid-template-columns: 232px minmax(0, 1fr); min-height: 100vh; min-height: 100dvh; }
  .rail { background: var(--panel); padding: 28px 20px 20px; display: flex; flex-direction: column; gap: 28px;
          position: sticky; top: 0; height: 100vh; height: 100dvh; }
  .word { font-size: 30px; font-weight: 800; letter-spacing: -0.03em; line-height: 1; margin: 0; }
  .how { font-size: 14px; color: var(--soft); margin: 10px 0 0; }
  .how b { color: var(--ink); font-weight: 600; }
  .how span { display: block; }
  .dot { display: inline-block; width: 9px; height: 9px; border-radius: 50%; background: var(--soft); margin-right: 7px; }
  .dot.on { background: var(--act); }
  .tabs { display: flex; flex-direction: column; gap: 2px; }
  .tabs button { background: none; border: 0; text-align: left; padding: 9px 12px; border-radius: 10px;
                 font-weight: 600; color: var(--soft); }
  .tabs button[aria-current="page"] { background: var(--paper); color: var(--ink); }
  .rail .foot { margin-top: auto; font-size: 14px; }
  .link { background: none; border: 0; padding: 0; color: var(--act); font-weight: 600; text-decoration: underline;
          text-underline-offset: 3px; }
  main { padding: 36px 48px 0; display: flex; flex-direction: column; min-width: 0; }
  .col { width: 100%; max-width: 660px; }
  h1 { font-size: 24px; letter-spacing: -0.02em; line-height: 1.2; margin: 0 0 6px; }
  .lede { color: var(--soft); margin: 0 0 28px; max-width: 56ch; }

  /* activity */
  .feed { flex: 1; padding-bottom: 24px; }
  .day { font-size: 14px; color: var(--soft); margin: 28px 0 14px; font-weight: 600; }
  .day:first-child { margin-top: 0; }
  .job { margin: 0 0 30px; }
  .ask { margin: 0; font-size: 19px; line-height: 1.55; font-weight: 600; overflow-wrap: anywhere; }
  .ask span { background: linear-gradient(transparent 10%, var(--mark) 10% 90%, transparent 90%); color: var(--mark-ink);
              padding: 0 5px; margin: 0 -5px; -webkit-box-decoration-break: clone; box-decoration-break: clone; }
  .when { font-size: 13px; color: var(--soft); margin: 4px 0 10px; }
  .said { margin: 0 0 10px; max-width: 58ch; overflow-wrap: anywhere; }
  .busy { color: var(--soft); display: flex; align-items: center; gap: 10px; margin: 0; }
  .breath { width: 12px; height: 12px; border-radius: 50%; background: var(--act); flex: none;
            animation: breathe 3.2s ease-in-out infinite; }
  @keyframes breathe { 0%, 100% { transform: scale(0.6); opacity: 0.45; } 50% { transform: scale(1); opacity: 1; } }
  @media (prefers-reduced-motion: reduce) { .breath { animation: none; } }
  .bad { color: var(--bad); }
  .row { display: flex; flex-wrap: wrap; align-items: center; gap: 8px 14px; }
  .btn { border: 1.5px solid var(--ink); background: none; border-radius: 999px; padding: 7px 16px; font-weight: 700; }
  .btn.go { background: var(--act); border-color: var(--act); color: var(--act-ink); }
  .btn:disabled { opacity: 0.45; cursor: default; }
  .note { font-size: 13px; color: var(--soft); }
  .empty { color: var(--soft); max-width: 50ch; }

  /* composer */
  .compose { position: sticky; bottom: 0; background: var(--paper); padding: 12px 0 22px; border-top: 1px solid var(--line); }
  .compose form { display: flex; gap: 10px; align-items: flex-end; }
  .compose textarea { flex: 1; min-width: 0; resize: none; border: 1.5px solid var(--line); border-radius: 16px;
                      background: var(--paper); padding: 10px 14px; line-height: 1.4; max-height: 9em; }
  .compose textarea:focus { border-color: var(--act); outline: none; }
  .compose .note { margin: 8px 2px 0; }

  /* reading a write-up */
  .back { margin: 0 0 22px; }
  .reading { font-family: var(--read); font-size: 18px; line-height: 1.62; overflow-wrap: anywhere; padding-bottom: 48px; }
  .reading h1 { font-family: var(--ui); font-size: 28px; line-height: 1.18; margin: 26px 0 14px; }
  .reading h2 { font-family: var(--ui); font-size: 19px; margin: 30px 0 6px; letter-spacing: -0.01em; }
  .reading h3 { font-family: var(--ui); font-size: 16px; margin: 22px 0 4px; }
  .reading p, .reading ul, .reading ol { margin: 10px 0; }
  .reading li { margin: 4px 0; }
  .reading a { color: var(--act); text-underline-offset: 3px; }
  .reading code { font-size: 0.86em; background: var(--panel); padding: 1px 5px; border-radius: 5px; }
  .reading hr { border: 0; border-top: 1px solid var(--line); margin: 26px 0; }
  .scroll { overflow-x: auto; margin: 14px 0; }
  table { border-collapse: collapse; font: 15px/1.4 var(--ui); }
  th, td { border-bottom: 1px solid var(--line); padding: 8px 16px 8px 0; text-align: left; vertical-align: top;
           overflow-wrap: normal; min-width: 5.5em; }
  th { font-weight: 700; border-bottom: 1.5px solid var(--ink); }

  /* ideas and memory */
  .list { list-style: none; margin: 0 0 34px; padding: 0; border-top: 1px solid var(--line); }
  .list li { border-bottom: 1px solid var(--line); }
  .idea { display: block; width: 100%; text-align: left; background: none; border: 0; padding: 13px 0; font-weight: 600; }
  .idea:hover { color: var(--act); }
  h2 { font-size: 17px; margin: 0 0 10px; letter-spacing: -0.01em; }
  .say { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 4px 20px; padding: 11px 0; }
  .say q { font-weight: 600; quotes: "\201C" "\201D"; }
  .say span { color: var(--soft); }
  .mem { display: flex; gap: 12px; align-items: baseline; justify-content: space-between; padding: 12px 0; }
  .mem span { overflow-wrap: anywhere; }
  .add { display: flex; gap: 10px; margin: 0 0 12px; }
  .add input, .sign input { flex: 1; min-width: 0; border: 1.5px solid var(--line); border-radius: 999px;
                            background: var(--paper); padding: 9px 16px; }
  .add input:focus, .sign input:focus { border-color: var(--act); outline: none; }
  .sign { display: flex; gap: 10px; flex-wrap: wrap; margin: 14px 0 8px; max-width: 460px; }
  code { font-family: ui-monospace, "SF Mono", Menlo, monospace; font-size: 0.88em; }

  @media (max-width: 760px) {
    .frame { display: block; }
    .rail { position: static; height: auto; flex-direction: row; align-items: center; gap: 14px; padding: 14px 16px; }
    .word { font-size: 24px; }
    .how { margin: 0; flex: 1; min-width: 0; text-align: right; }
    .how span { display: inline; }
    .how .extra { display: none; }
    .rail .foot { display: none; }
    .tabs { position: fixed; left: 0; right: 0; bottom: 0; z-index: 2; flex-direction: row; background: var(--panel);
            padding: 6px 10px calc(6px + env(safe-area-inset-bottom)); gap: 6px; }
    .tabs button { flex: 1; text-align: center; padding: 10px 4px; }
    main { padding: 22px 16px 0; min-height: calc(100dvh - 60px); }
    .compose { bottom: calc(52px + env(safe-area-inset-bottom)); padding-bottom: 12px; }
    body { padding-bottom: calc(56px + env(safe-area-inset-bottom)); }
    .say { grid-template-columns: 1fr; }
    .reading { font-size: 17px; }
  }
</style>
</head>
<body>
<div class="frame">
  <aside class="rail">
    <div>
      <p class="word">muse</p>
    </div>
    <p class="how" id="how" aria-live="polite"></p>
    <nav class="tabs" aria-label="Sections">
      <button data-go="activity">Activity</button>
      <button data-go="ideas">Ideas</button>
      <button data-go="memory">Memory</button>
    </nav>
    <p class="foot" id="foot"></p>
  </aside>
  <main>
    <div class="col feed" id="view"></div>
    <div class="col compose" id="compose" hidden>
      <form id="askform">
        <textarea id="task" rows="1" aria-label="What should Muse look into?" placeholder="Ask Muse to look into something"></textarea>
        <button class="btn go" id="askbtn">Ask</button>
      </form>
      <p class="note" id="asknote" aria-live="polite"></p>
    </div>
  </main>
</div>
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

let token = "";
try {
  if (location.hash.length > 1 && !location.hash.startsWith("#/")) {
    localStorage.setItem("muse_token", decodeURIComponent(location.hash.slice(1)));
    history.replaceState(null, "", location.pathname);
  }
  token = localStorage.getItem("muse_token") || "";
} catch (e) {}

const $ = id => document.getElementById(id);
const view = $("view"), compose = $("compose"), task = $("task");
let jobs = [], app = null, canRead = true, drawn = "", lastCount = -1, notice = "";

const esc = s => String(s).replace(/[&<>"']/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[c]));

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

function timeOf(seconds) {
  return new Date(seconds * 1000).toLocaleTimeString(undefined, {hour: "numeric", minute: "2-digit"});
}
function dayOf(seconds) {
  const d = new Date(seconds * 1000), now = new Date();
  const days = Math.round((new Date(now.getFullYear(), now.getMonth(), now.getDate()) - new Date(d.getFullYear(), d.getMonth(), d.getDate())) / 864e5);
  return days === 0 ? "Today" : days === 1 ? "Yesterday" : d.toLocaleDateString(undefined, {weekday: "long", day: "numeric", month: "long"});
}
const sentence = s => { s = String(s || "").trim(); return s ? s.charAt(0).toUpperCase() + s.slice(1) + (/[.!?]$/.test(s) ? "" : ".") : ""; };
const minutes = s => s < 90 ? "about a minute" : Math.round(s / 60) + " minutes";

function signIn(why) {
  return `<h1>Sign in</h1><p class="lede">${why}</p>
    <p>On your computer, <code>python3 companion/muse_companion.py results</code> opens this page signed in.
    Or paste the token from <code>~/.config/muse/server.json</code>.</p>
    <form class="sign" id="signform"><input type="password" id="tok" aria-label="Token" autocomplete="off" placeholder="Token">
    <button class="btn go">Sign in</button></form><p class="note bad" id="signnote" aria-live="polite"></p>`;
}

function jobHtml(j) {
  let body;
  if (j.status === "running") {
    body = `<p class="busy"><i class="breath"></i>Looking into it. This takes a few minutes.</p>`;
  } else if (j.status === "done") {
    body = `<p class="said">${esc(j.summary || "The write-up is ready.")}</p>
      <div class="row"><button class="btn" data-read="${esc(j.id)}">Read the write-up</button>
      <span class="note">${j.words ? j.words + " words, " : ""}${j.took_s ? minutes(j.took_s) + " of work" : ""}</span></div>`;
  } else {
    body = `<p class="said bad">This did not finish. ${esc(sentence(j.error))}</p>
      <div class="row"><button class="btn" data-again="${esc(j.id)}">Ask again</button></div>`;
  }
  return `<section class="job"><p class="ask"><span>${esc(j.task || j.title)}</span></p>
    <p class="when">${timeOf(j.created)}</p>${body}</section>`;
}

function drawActivity() {
  if (!canRead) return signIn("Your requests and Muse's write-ups are private on this server.");
  if (!jobs.length) {
    return `<h1>Nothing yet</h1><p class="empty">Hold BOOT and ask Muse to look into something, or type it below.
      Muse says when it is ready, and the write-up appears here. <button class="link" data-go="ideas">See some ideas</button></p>`;
  }
  let html = "", day = "";
  for (const j of jobs.slice().reverse()) {
    const d = dayOf(j.created);
    if (d !== day) { html += `<p class="day">${d}</p>`; day = d; }
    html += jobHtml(j);
  }
  return html;
}

function drawIdeas() {
  return `<h1>Ideas</h1><p class="lede">Pick one to start from. You can change the words before you ask.</p>
    <h2>Hand it to Muse</h2>
    <ul class="list">${IDEAS.map(t => `<li><button class="idea" data-idea="${esc(t)}">${esc(t)}</button></li>`).join("")}</ul>
    <h2>Say it out loud</h2>
    <p class="note" style="margin:-4px 0 10px">Hold the BOOT button on Muse, speak, then let go.</p>
    <ul class="list">${SAY.map(([q, what]) => `<li class="say"><q>${esc(q)}</q><span>${esc(what)}</span></li>`).join("")}</ul>`;
}

function drawMemory() {
  if (!app) return signIn("What Muse remembers about you is private.");
  const notes = app.memory || [];
  return `<h1>Memory</h1><p class="lede">What Muse remembers about you. It uses these when you talk, and hands them to its research.</p>
    <form class="add" id="addform"><input id="note" aria-label="Something for Muse to remember" maxlength="160"
      placeholder="Something for Muse to remember"><button class="btn go">Remember</button></form>
    <p class="note" id="memnote" aria-live="polite">${notice}</p>
    ${notes.length ? `<ul class="list">${notes.map(n => `<li class="mem"><span>${esc(n)}</span>
      <button class="link" data-forget="${esc(n)}">Forget</button></li>`).join("")}</ul>`
      : `<p class="empty">Nothing yet. Tell Muse out loud ("remember that I like green tea") or add a note here.</p>`}`;
}

async function drawReader(id) {
  let j;
  try { j = await call("/results/" + encodeURIComponent(id) + ".json"); }
  catch (e) { return e.message === "signin" ? signIn("This write-up is private on this server.") : `<p class="empty">Could not open that one. ${esc(e.message)}</p>`; }
  const top = `<p class="back"><button class="link" data-go="activity">Back to activity</button></p>
    <p class="ask"><span>${esc(j.task || j.title)}</span></p><p class="when">${dayOf(j.created)}, ${timeOf(j.created)}</p>`;
  if (j.status === "running") return top + `<p class="busy"><i class="breath"></i>Muse is still working on this.</p>`;
  if (j.status !== "done") return top + `<p class="said bad">This did not finish. ${esc(sentence(j.error))}</p>`;
  return top + `<div class="reading">${markdown(j.report || "")}</div>`;
}

function drawRail() {
  const tab = route()[0] === "read" ? "activity" : route()[0];
  for (const b of document.querySelectorAll(".tabs button")) b.setAttribute("aria-current", b.dataset.go === tab ? "page" : "false");
  let how = "", foot = "";
  if (app) {
    const m = app.muse;
    how = `<span><i class="dot ${app.connected ? "on" : ""}"></i><b>${app.connected ? "Muse is connected" : "Muse is not connected"}</b></span>`;
    if (m && m.battery_percent != null) how += `<span class="extra">Battery ${m.battery_percent}%${m.power === "running on battery" ? "" : ", " + esc(m.power)}</span>`;
    for (const t of app.timers || []) how += `<span class="extra">${esc(t.label)} in ${t.minutes_left < 1 ? "under a minute" : Math.round(t.minutes_left) + " min"}</span>`;
    if (token) foot = `<button class="link" id="signout">Sign out</button>`;
  } else if (canRead) {
    foot = `<button class="link" data-go="memory">Sign in</button>`;
  }
  $("how").innerHTML = how;
  $("foot").innerHTML = foot;
}

async function draw(force) {
  const [where, id] = route();
  drawRail();
  let html, key;
  if (where === "read" && id) { key = "read" + id + (jobs.find(j => j.id === id) || {}).status; if (key === drawn && !force) return; html = await drawReader(id); }
  else if (where === "ideas") { key = "ideas"; html = drawIdeas(); }
  else if (where === "memory") { key = "memory" + JSON.stringify(app && app.memory) + notice; html = drawMemory(); }
  else { key = "activity" + canRead + JSON.stringify(jobs.map(j => [j.id, j.status])); html = drawActivity(); }
  const asking = where === "activity" && canRead;
  compose.hidden = !asking;
  if (asking) {
    const why = !app ? "" : app.cannot_work;
    $("askbtn").disabled = !app || Boolean(why);
    task.disabled = !app;
    $("asknote").innerHTML = !app ? `<button class="link" data-go="memory">Sign in</button> to ask from here. You can always ask Muse out loud.`
      : esc(sentence(why));
  }
  if (key === drawn && !force) return;
  const fresh = !drawn.startsWith(where === "read" ? "read" + id : where);
  drawn = key;
  view.innerHTML = html;
  if (where === "activity" && (fresh || jobs.length !== lastCount)) window.scrollTo(0, document.documentElement.scrollHeight);
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
  if (!text) return;
  $("askbtn").disabled = true;
  try {
    const r = await call("/ask", {task: text});
    if (r.ok) { task.value = ""; grow(); $("asknote").textContent = ""; go("activity"); }
    else $("asknote").textContent = r.error || "Muse could not start that.";
  } catch (e) { $("asknote").textContent = e.message === "signin" ? "Sign in to ask from here." : e.message; }
  await refresh();
}

function grow() { task.style.height = "auto"; task.style.height = Math.min(task.scrollHeight + 3, 150) + "px"; }

document.addEventListener("click", async e => {
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
  else if (b.id === "signout") { token = ""; try { localStorage.removeItem("muse_token"); } catch (err) {} drawn = ""; await refresh(); }
});

document.addEventListener("submit", async e => {
  e.preventDefault();
  if (e.target.id === "askform") ask(task.value);
  else if (e.target.id === "addform") {
    const text = $("note").value.trim();
    if (!text) return;
    const r = await call("/memory", {add: text}).catch(err => ({error: err.message}));
    notice = r.ok ? "" : esc(r.error || "Could not save that.");
    await refresh();
  } else if (e.target.id === "signform") {
    token = $("tok").value.trim();
    try { await call("/app.json"); try { localStorage.setItem("muse_token", token); } catch (err) {} drawn = ""; await refresh(); }
    catch (err) { token = ""; $("signnote").textContent = err.message === "signin" ? "That token was not accepted." : err.message; }
  }
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
