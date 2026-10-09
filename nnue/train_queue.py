"""Run a manifest of training experiments sequentially on the one GPU.

Phase 7 runs many training jobs (learning-curve sizes, sampling variants,
loss/target variants, teacher rungs). They all share one RTX 4050, so they must
be serialized, and the machine has already proved it can die mid-run: a queue
that skips jobs whose curve JSON already exists makes a restart a no-op for the
finished ones and a resume for the rest.

The manifest is data, not code:

    {"runs": [
      {"tag": "NNUE-DATA-001", "curve": "nnue/curves/NNUE-DATA-001.json",
       "args": ["--data", "nnue/data/v4_sub100k.json", "--max-records", "100000",
                "--tag", "NNUE-DATA-001", "--epochs", "20", ...]},
      ...
    ]}

    py -3 -u nnue/train_queue.py --manifest nnue/phase7_queue.json
    py -3 -u nnue/train_queue.py --manifest nnue/phase7_queue.json --only TAG
"""

import argparse
import json
import os
import subprocess
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--only", default=None, help="run only this tag")
    ap.add_argument("--list", action="store_true", help="print the manifest and exit")
    args = ap.parse_args()

    with open(args.manifest, "r", encoding="utf-8") as f:
        manifest = json.load(f)

    if args.list:
        for run in manifest["runs"]:
            state = "done" if os.path.exists(run["curve"]) else "pending"
            print(f"{run['tag']:20s} {state:8s} {run['curve']}")
        return 0

    status_path = os.path.splitext(args.manifest)[0] + "_status.json"
    status = {"started": time.strftime("%Y-%m-%dT%H:%M:%S"), "runs": {}}
    if os.path.exists(status_path):
        with open(status_path, "r", encoding="utf-8") as f:
            previous = json.load(f)
        status["runs"] = previous.get("runs", {})

    for run in manifest["runs"]:
        tag = run["tag"]
        if args.only and tag != args.only:
            continue
        curve = run["curve"]
        if os.path.exists(curve):
            print(f"[queue] {tag}: curve exists, skipping", flush=True)
            continue
        os.makedirs(os.path.dirname(curve) or ".", exist_ok=True)
        log_path = os.path.splitext(curve)[0] + ".log"
        cmd = [sys.executable, "-u", "nnue/train.py"] + run["args"] + \
            ["--curve-out", curve]
        entry = {"cmd": cmd, "started": time.strftime("%Y-%m-%dT%H:%M:%S")}
        status["runs"][tag] = entry
        with open(status_path, "w", encoding="utf-8") as f:
            json.dump(status, f, indent=2)
        print(f"[queue] {tag}: starting", flush=True)
        t0 = time.time()
        with open(log_path, "w", encoding="utf-8") as log:
            log.write(f"$ {' '.join(cmd)}\n")
            log.flush()
            proc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
        entry["exit_code"] = proc.returncode
        entry["seconds"] = round(time.time() - t0, 1)
        entry["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        status["runs"][tag] = entry
        with open(status_path, "w", encoding="utf-8") as f:
            json.dump(status, f, indent=2)
        print(f"[queue] {tag}: exit={proc.returncode} in {entry['seconds']}s",
              flush=True)
        if proc.returncode != 0:
            print(f"[queue] {tag} failed; continuing with the next run",
                  flush=True)

    status["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S")
    with open(status_path, "w", encoding="utf-8") as f:
        json.dump(status, f, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
