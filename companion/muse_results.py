# SPDX-License-Identifier: MIT
"""The results page: where you read what Muse's background jobs wrote.

    GET /results              this page (it holds no data itself)
    GET /results.json         the jobs, newest first, without their write-ups
    GET /results/<id>.json    one job with its write-up

Online, the two JSON addresses need the server's token. The page keeps the
token in the browser: `muse_companion.py results` opens the page with the
token after the "#" of the address (that part is never sent to a server), and
the page stores it and removes it from the address bar.
"""

PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="robots" content="noindex">
<title>Muse results</title>
<style>
  :root {
    --bg: #f6f3ee; --panel: #fffdf9; --ink: #2b2622; --soft: #6f665e; --line: #e4ddd3;
    --accent: #b4552d; --ok: #2f7a4d; --bad: #b3261e; --wait: #8a6d1a;
  }
  @media (prefers-color-scheme: dark) {
    :root { --bg: #1b1917; --panel: #242120; --ink: #ece6df; --soft: #a59c93; --line: #37322e;
            --accent: #e58a5f; --ok: #6cc08b; --bad: #f08b84; --wait: #d9b85a; }
  }
  * { box-sizing: border-box; }
  body { margin: 0; background: var(--bg); color: var(--ink);
         font: 16px/1.55 system-ui, -apple-system, "Segoe UI", sans-serif; }
  header { padding: 20px 16px 8px; max-width: 1080px; margin: 0 auto; }
  header h1 { margin: 0; font-size: 22px; letter-spacing: -0.01em; }
  header p { margin: 2px 0 0; color: var(--soft); font-size: 14px; }
  main { display: grid; grid-template-columns: 300px 1fr; gap: 16px; max-width: 1080px;
         margin: 0 auto; padding: 8px 16px 40px; align-items: start; }
  @media (max-width: 760px) { main { grid-template-columns: 1fr; } }
  nav { display: flex; flex-direction: column; gap: 8px; }
  nav button { text-align: left; background: var(--panel); color: inherit; border: 1px solid var(--line);
               border-radius: 10px; padding: 10px 12px; font: inherit; cursor: pointer; }
  nav button[aria-current="true"] { border-color: var(--accent); box-shadow: inset 3px 0 0 var(--accent); }
  nav button:focus-visible, a:focus-visible, input:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
  .title { display: block; font-weight: 600; overflow-wrap: anywhere; }
  .meta { display: block; font-size: 13px; color: var(--soft); }
  .done { color: var(--ok); } .failed { color: var(--bad); } .running { color: var(--wait); }
  article { background: var(--panel); border: 1px solid var(--line); border-radius: 12px;
            padding: 20px 22px; min-height: 200px; overflow-wrap: anywhere; }
  article h1 { font-size: 24px; line-height: 1.25; margin: 0 0 12px; }
  article h2 { font-size: 18px; margin: 24px 0 6px; }
  article h3 { font-size: 16px; margin: 18px 0 4px; }
  article p, article ul, article ol { margin: 8px 0; }
  article a { color: var(--accent); }
  article code { background: var(--bg); padding: 1px 5px; border-radius: 4px; font-size: 0.92em; }
  article hr { border: 0; border-top: 1px solid var(--line); margin: 18px 0; }
  .said { border-left: 3px solid var(--accent); padding: 2px 0 2px 12px; margin: 0 0 16px; color: var(--soft); }
  .asked { font-size: 14px; color: var(--soft); margin: 0 0 14px; }
  .scroll { overflow-x: auto; }
  table { border-collapse: collapse; font-size: 14.5px; margin: 10px 0; }
  th, td { border: 1px solid var(--line); padding: 6px 10px; text-align: left; vertical-align: top;
           overflow-wrap: normal; min-width: 5em; }
  th { background: var(--bg); }
  .empty { color: var(--soft); }
  form { display: flex; gap: 8px; flex-wrap: wrap; margin-top: 12px; }
  input { flex: 1; min-width: 200px; padding: 8px 10px; border-radius: 8px; border: 1px solid var(--line);
          background: var(--bg); color: inherit; font: inherit; }
  form button { padding: 8px 14px; border-radius: 8px; border: 0; background: var(--accent); color: #fff;
                font: inherit; cursor: pointer; }
</style>
</head>
<body>
<header>
  <h1>Muse results</h1>
  <p>What Muse worked on in the background. Ask Muse out loud; read the write-up here.</p>
</header>
<main>
  <nav id="list" aria-label="Jobs"></nav>
  <article id="view" aria-live="polite"><p class="empty">Loading...</p></article>
</main>
<script>
"use strict";
let token = "";
try {
  if (location.hash.length > 1) {
    localStorage.setItem("muse_token", decodeURIComponent(location.hash.slice(1)));
    history.replaceState(null, "", location.pathname);
  }
  token = localStorage.getItem("muse_token") || "";
} catch (e) { token = location.hash.slice(1); }

const list = document.getElementById("list"), view = document.getElementById("view");
let jobs = [], current = "", shown = "";

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
      const ordered = /\d/.test(m[1]), tag = ordered ? "ol" : "ul";
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

function when(seconds) {
  const d = new Date(seconds * 1000), today = new Date().toDateString() === d.toDateString();
  return (today ? "" : d.toLocaleDateString(undefined, {day: "numeric", month: "short"}) + ", ") +
         d.toLocaleTimeString(undefined, {hour: "numeric", minute: "2-digit"});
}

const label = job => job.status === "running" ? "Working on it" : job.status === "done" ? "Ready" : "Did not finish";

async function get(path) {
  const r = await fetch(path, {headers: token ? {Authorization: "Bearer " + token} : {}, cache: "no-store"});
  if (r.status === 401 || r.status === 403) throw new Error("token");
  if (!r.ok) throw new Error("The server answered " + r.status + ".");
  return r.json();
}

function askToken(wrong) {
  list.innerHTML = "";
  view.innerHTML = `<h2>${wrong ? "That token was not accepted" : "This page needs your token"}</h2>
    <p>On your computer, <code>python3 companion/muse_companion.py results</code> opens this page signed in.
    Or paste the token from <code>~/.config/muse/server.json</code>:</p>
    <form><input type="password" aria-label="Token" autocomplete="off" placeholder="Token"><button>Open</button></form>`;
  view.querySelector("form").addEventListener("submit", e => {
    e.preventDefault();
    token = view.querySelector("input").value.trim();
    try { localStorage.setItem("muse_token", token); } catch (err) {}
    refresh();
  });
}

function drawList() {
  list.innerHTML = jobs.map(j => `<button data-id="${esc(j.id)}" aria-current="${j.id === current}">
      <span class="title">${esc(j.title)}</span>
      <span class="meta"><span class="${esc(j.status)}">${label(j)}</span> &middot; ${when(j.created)}</span>
    </button>`).join("");
  for (const b of list.querySelectorAll("button")) b.addEventListener("click", () => open(b.dataset.id));
}

async function open(id) {
  current = id;
  drawList();
  let job;
  try { job = await get("/results/" + encodeURIComponent(id) + ".json"); }
  catch (e) { view.innerHTML = `<p class="empty">Could not load that one. ${esc(e.message)}</p>`; return; }
  const key = id + job.status;
  if (key === shown) return;
  shown = key;
  let html = `<p class="asked">You asked: ${esc(job.task)}</p>`;
  if (job.status === "running") html += `<p class="running">Muse is still working on this. This page updates by itself.</p>`;
  else if (job.status === "failed") html += `<p class="failed">This did not finish. ${esc(job.error || "")}</p>`;
  else html += (job.summary ? `<p class="said">Muse said: ${esc(job.summary)}</p>` : "") + markdown(job.report || "");
  view.innerHTML = html;
}

async function refresh() {
  try { jobs = (await get("/results.json")).jobs || []; }
  catch (e) {
    if (e.message === "token") askToken(Boolean(token));
    else view.innerHTML = `<p class="empty">Could not reach the server. ${esc(e.message)}</p>`;
    return;
  }
  if (!jobs.length) {
    list.innerHTML = "";
    view.innerHTML = `<p class="empty">Nothing yet. Hold BOOT and ask Muse to look into something, for example
      "research the best budget 3D printers".</p>`;
    return;
  }
  if (!jobs.some(j => j.id === current)) current = jobs[0].id;
  await open(current);
}

refresh();
setInterval(() => { if (!document.hidden && !view.querySelector("form")) refresh(); }, 8000);
</script>
</body>
</html>
"""
