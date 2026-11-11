# SPDX-License-Identifier: MIT
"""One background job for Muse: research a question on the web and write it up.

The companion server (muse_jobs.py) starts this as its own process, so the
server itself stays small and keeps answering Muse while the job runs:

    python3 muse_worker.py /path/to/job.json

The job file holds {"task": "...", "model": "...", "about_user": [...],
"today": "..."}. The
result is written back into the same file: "status" ("done" or "failed"),
"summary" (one or two sentences for Muse to say) and "report" (markdown).

The agent is a Deep Agent (https://github.com/langchain-ai/deepagents): it
plans, searches the web with the model provider's own search tool, keeps notes
in a scratch file system that exists only in memory, and writes the report. It
has no shell and no access to this computer's files.

Needs: pip install deepagents langchain-anthropic, and ANTHROPIC_API_KEY.
MUSE_WORKER_FAKE=1 skips the model and returns a canned result (for testing).
"""

import json
import os
import sys
import time

MODEL = "anthropic:claude-sonnet-5-5"
MAX_SEARCHES = 8
MAX_STEPS = 60       # model and tool turns before the job is stopped
SUMMARY_MAX = 220    # characters Muse will say

INSTRUCTIONS = """You are the research helper of Muse, a small desk companion. Its owner asked for \
something out loud and will read your write-up later on a web page; Muse will say a short summary.

Work like a careful researcher: plan briefly, search the web for current information, read enough \
sources to be sure, and prefer primary and recent sources. Do not ask questions back; make sensible \
assumptions and state them.

Your final message must have exactly this shape:

SUMMARY: one or two short, plain sentences with the answer itself, at most 200 characters, \
no markdown, written to be spoken aloud.

Then a blank line, then the write-up in markdown: a title line starting with "# ", the answer \
first, then the details in short sections, a comparison table if it helps, and a "Sources" list \
of links at the end. Keep it under about 600 words."""


def text_of(content):
    """The text of a model message, whose content may be a list of blocks."""
    if isinstance(content, str):
        return content
    parts = []
    for block in content or []:
        if isinstance(block, str):
            parts.append(block)
        elif isinstance(block, dict) and block.get("type") == "text":
            parts.append(block.get("text", ""))
    return "".join(parts)


def split_answer(answer):
    """(summary, report) from the agent's final message."""
    answer = answer.strip()
    summary, report = "", answer
    if answer.upper().startswith("SUMMARY:"):
        head, _, rest = answer[len("SUMMARY:"):].partition("\n")
        summary, report = head.strip(), rest.strip()
    if not summary:
        # No summary line: the first sentence of the report will have to do.
        plain = " ".join(line for line in report.splitlines() if line and not line.startswith(("#", "|", "-")))
        summary = plain.split(". ")[0].strip()
    if len(summary) > SUMMARY_MAX:
        summary = summary[:SUMMARY_MAX].rsplit(" ", 1)[0] + "..."
    return summary, report or answer


def research(job):
    from deepagents import create_deep_agent
    from deepagents.backends import StateBackend

    search = {"type": "web_search_20260209", "name": "web_search",
              "max_uses": int(job.get("max_searches") or MAX_SEARCHES)}
    # StateBackend: the agent's files live in memory only, and it cannot run commands.
    agent = create_deep_agent(model=job.get("model") or MODEL, tools=[search], system_prompt=INSTRUCTIONS,
                              backend=StateBackend())
    task = job["task"]
    if job.get("today"):
        task += "\n\nToday is %s." % job["today"]
    if job.get("about_user"):
        task += ("\n\nWhat Muse remembers about its owner (use what is relevant; do not repeat it back):\n" +
                 "\n".join("- %s" % note for note in job["about_user"]))
    result = agent.invoke({"messages": [{"role": "user", "content": task}]},
                          {"recursion_limit": MAX_STEPS})
    return text_of(result["messages"][-1].content)


def fake(job):
    time.sleep(float(os.environ.get("MUSE_WORKER_FAKE_S") or 3))
    if "fail" in job["task"].lower():
        raise RuntimeError("asked to fail")
    return ("SUMMARY: This is a stand-in result for: %s.\n\n# Stand-in result\n\nNo model was used.\n\n"
            "## Sources\n\n- [Example](https://example.com)" % job["task"][:80])


def main():
    path = sys.argv[1]
    with open(path) as f:
        job = json.load(f)
    started = time.time()
    try:
        answer = fake(job) if os.environ.get("MUSE_WORKER_FAKE") else research(job)
        summary, report = split_answer(answer)
        job.update(status="done", summary=summary, report=report)
    except Exception as e:  # whatever went wrong, the job file says so
        job.update(status="failed", error=("%s: %s" % (type(e).__name__, e))[:400])
    job["took_s"] = round(time.time() - started)
    with open(path + ".tmp", "w") as f:
        json.dump(job, f, indent=1)
    os.replace(path + ".tmp", path)


if __name__ == "__main__":
    main()
