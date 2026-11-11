# SPDX-License-Identifier: MIT
"""Background jobs: work that takes minutes, done while Muse gets on with its day.

You ask Muse for something bigger than a quick answer ("research the best
budget 3D printers"). The AI calls its work_on tool, which lands in
Jobs.start() here: the job is written to a file and muse_worker.py is started
as its own process to do it. When that process ends, `on_done` is called and
Muse says the result is ready; the write-up is read on the server's /results
page (muse_results.py).

Jobs are kept as JSON files in jobs/ under ~/.config/muse (or MUSE_DATA_DIR):
{"id", "title", "task", "status": "running|done|failed", "created",
 "summary", "report", "error", "took_s"}.

The worker needs an Anthropic key: ANTHROPIC_API_KEY in the repository's .env
file (muse_env.py) or in the environment. It runs with this Python if the
deepagents package is installed for it, or else through uv, which fetches the
packages by itself. MUSE_WORKER_PYTHON names another Python to use.
"""

import asyncio
import importlib.util
import json
import os
import secrets
import shutil
import sys
import time

import muse_env

HERE = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.environ.get("MUSE_DATA_DIR") or os.path.expanduser("~/.config/muse")
JOBS_DIR = os.path.join(DATA_DIR, "jobs")
KEY_FILE = os.path.expanduser("~/.config/muse/anthropic_key")
WORKER = os.path.join(HERE, "muse_worker.py")
PACKAGES = ["deepagents", "langchain-anthropic"]

JOB_MAX_S = 12 * 60     # a job that runs longer is stopped
AT_ONCE = 2             # jobs running at the same time
KEEP = 30               # finished jobs kept on disk
TASK_MAX = 2000


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def anthropic_key():
    key = muse_env.get("ANTHROPIC_API_KEY")
    if not key:                      # where the key was kept before .env
        try:
            with open(KEY_FILE) as f:
                key = f.read().strip()
        except OSError:
            pass
    return key if len(key) >= 20 and not any(c.isspace() for c in key) else ""


def worker_command():
    """How to run muse_worker.py, or None when the packages it needs are missing."""
    if os.environ.get("MUSE_WORKER_PYTHON"):
        return [os.environ["MUSE_WORKER_PYTHON"], WORKER]
    if os.environ.get("MUSE_WORKER_FAKE") or importlib.util.find_spec("deepagents"):
        return [sys.executable, WORKER]
    uv = shutil.which("uv") or next((p for p in ("/opt/homebrew/bin/uv", os.path.expanduser("~/.local/bin/uv"))
                                     if os.path.exists(p)), None)
    if uv:
        with_packages = [arg for name in PACKAGES for arg in ("--with", name)]
        return [uv, "run", "--no-project", "--quiet", "--python", "3.12"] + with_packages + ["python", WORKER]
    return None


def _path(job_id):
    return os.path.join(JOBS_DIR, job_id + ".json")


def _read(path):
    try:
        with open(path) as f:
            job = json.load(f)
        return job if isinstance(job, dict) and job.get("id") else None
    except (OSError, ValueError):
        return None


def _write(job):
    os.makedirs(JOBS_DIR, exist_ok=True)
    path = _path(job["id"])
    with open(path + ".tmp", "w") as f:
        json.dump(job, f, indent=1)
    os.replace(path + ".tmp", path)


class Jobs:
    def __init__(self, on_done):
        self.on_done = on_done   # called with each job as it finishes or fails
        self.running = {}        # id -> asyncio task
        # Jobs a previous server left running were lost with it.
        for job in self.list():
            if job.get("status") == "running":
                job.update(status="failed", error="the server restarted while this was running")
                _write(job)

    def problem(self):
        """Why a job cannot be started right now, or None."""
        if not os.environ.get("MUSE_WORKER_FAKE") and not anthropic_key():
            return "background research is not set up yet: ANTHROPIC_API_KEY is missing from .env"
        if not worker_command():
            return "background research is not set up yet: pip install " + " ".join(PACKAGES)
        if len(self.running) >= AT_ONCE:
            return "already working on %d things; wait for one to finish" % len(self.running)
        return None

    def start(self, task, title="", model="", about=None, today=""):
        """Start a job. `about` is what Muse remembers about its owner and `today`
        the date where they are, both handed to the worker."""
        task = " ".join(str(task or "").split())[:TASK_MAX]
        if len(task) < 8:
            return {"ok": False, "error": "say what to work on, in a full sentence"}
        problem = self.problem()
        if problem:
            log("job not started:", problem)
            return {"ok": False, "started": False, "error": problem,
                    "note": "Nothing was started. Tell the user in one sentence that you cannot do this "
                            "right now and why. Do not say you are working on it or that you will try again."}
        title = " ".join(str(title or "").split())[:60] or task[:60]
        job = {"id": secrets.token_hex(8), "title": title, "task": task, "status": "running",
               "created": time.time(), "model": model, "about_user": list(about or [])[:40], "today": today}
        _write(job)
        self.running[job["id"]] = asyncio.ensure_future(self._run(job))
        log("job started: %s (%s)" % (title, job["id"]))
        return {"ok": True, "started": title,
                "note": "this runs in the background for a few minutes; the user is told when it is ready"}

    async def _run(self, job):
        path, error = _path(job["id"]), ""
        env = dict(os.environ)
        if anthropic_key():
            env["ANTHROPIC_API_KEY"] = anthropic_key()
        try:
            proc = await asyncio.create_subprocess_exec(
                *worker_command(), path, env=env, cwd=HERE,
                stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.PIPE)
            try:
                _, err = await asyncio.wait_for(proc.communicate(), JOB_MAX_S)
                error = err.decode("utf-8", "replace").strip()[-300:]
            except asyncio.TimeoutError:
                proc.kill()
                error = "it took longer than %d minutes" % (JOB_MAX_S // 60)
        except (OSError, TypeError) as e:
            error = "could not start the worker: %s" % e
        done = _read(path) or job
        if done.get("status") == "running":   # the worker ended without an answer
            done.update(status="failed", error=error or "the worker stopped without a result")
            done.setdefault("took_s", round(time.time() - job["created"]))
            _write(done)
        self.running.pop(job["id"], None)
        log("job %s: %s (%s s)%s" % (done["status"], done["title"], done.get("took_s", "?"),
                                     "" if done["status"] == "done" else " - " + str(done.get("error"))[:200]))
        self._tidy()
        self.on_done(done)

    def _tidy(self):
        for job in self.list()[KEEP:]:
            try:
                os.remove(_path(job["id"]))
            except OSError:
                pass

    def list(self):
        """Every job, newest first."""
        try:
            names = [n for n in os.listdir(JOBS_DIR) if n.endswith(".json")]
        except OSError:
            return []
        jobs = [j for j in (_read(os.path.join(JOBS_DIR, n)) for n in names) if j]
        return sorted(jobs, key=lambda j: j.get("created", 0), reverse=True)

    def get(self, job_id):
        if not job_id.isalnum():
            return None
        return _read(_path(job_id))

    def brief(self, job):
        """A job without its write-up, for lists."""
        out = {k: job.get(k) for k in ("id", "title", "task", "status", "created", "took_s", "summary", "error")
               if job.get(k) is not None}
        if job.get("report"):
            out["words"] = len(job["report"].split())
        if job.get("status") == "running":
            out["running_for_s"] = round(time.time() - job.get("created", time.time()))
        return out

    def for_ai(self):
        """What the AI is told when it asks how the work is going."""
        jobs = []
        for i, job in enumerate(self.list()[:5]):
            item = {"title": job.get("title"), "status": job.get("status"),
                    "minutes_ago": round((time.time() - job.get("created", time.time())) / 60)}
            if job.get("summary"):
                item["summary"] = job["summary"]
            if job.get("error"):
                item["error"] = str(job["error"])[:160]
            if i == 0 and job.get("report"):
                item["write_up_start"] = job["report"][:900]
            jobs.append(item)
        return {"jobs": jobs, "note": "the full write-ups are on the results page"} if jobs else \
               {"jobs": [], "note": "nothing has been asked for yet"}
