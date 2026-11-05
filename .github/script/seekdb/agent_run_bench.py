#!/usr/bin/env python3
"""Start a local seekdb, run tools/benchmark/fts_large_bench.sh, then score it.

复用官方 .github/script/seekdb/mysqltest_for_seekdb.py 的部署与启动逻辑，
只在本地起一个 seekdb 实例跑官方基准脚本，再用官方打分脚本出分。

用法:
    python3 .github/script/seekdb/agent_run_bench.py \
        --seekdb build_release/src/observer/seekdb \
        --obclient deps/3rd/u01/obclient/bin/obclient \
        --work-dir /tmp/agent_bench --label before
"""
import argparse
import importlib.util
import os
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
RUNNER_PATH = Path(__file__).resolve().with_name("mysqltest_for_seekdb.py")


def load_runner():
    spec = importlib.util.spec_from_file_location("seekdb_mysqltest_runner", str(RUNNER_PATH))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seekdb", required=True)
    parser.add_argument("--obclient", required=True)
    parser.add_argument("--work-dir", default="/tmp/agent_bench")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--label", default="ci")
    parser.add_argument("--mysql", default="mysql")
    args = parser.parse_args()

    runner = load_runner()
    ns = argparse.Namespace()
    ns.base_dir = os.path.join(args.work_dir, "instance")
    ns.work_dir = args.work_dir
    ns.seekdb = Path(args.seekdb).resolve()
    ns.obclient = Path(args.obclient).resolve()
    ns.mysqltest = Path(args.obclient).resolve().parent / "mysqltest"
    ns.host = args.host
    ns.port = args.port

    deploy_dir = REPO_ROOT / "tools" / "deploy"
    sdb_script = RUNNER_PATH.with_name("sdb.py")
    bench_script = REPO_ROOT / "tools" / "benchmark" / "fts_large_bench.sh"
    score_script = REPO_ROOT / "tools" / "benchmark" / "fts_large_bench_score.py"
    report = Path(args.work_dir) / "bench_report.txt"

    try:
        runner.prepare_instance(ns, REPO_ROOT, sdb_script, deploy_dir)

        env = dict(os.environ)
        env["LABEL"] = args.label
        env["OUTPUT"] = str(report)
        env["MYSQL"] = "{} -h{} -P{} -uroot -N -s --default-character-set=utf8mb4".format(
            args.mysql, args.host, args.port)
        env["MYSQL_VERBOSE"] = "{} -h{} -P{} -uroot --default-character-set=utf8mb4".format(
            args.mysql, args.host, args.port)

        print("running benchmark: {}".format(bench_script), flush=True)
        proc = subprocess.run(["bash", str(bench_script)], cwd=str(REPO_ROOT), env=env)
        if proc.returncode != 0:
            print("benchmark failed with code {}".format(proc.returncode), file=sys.stderr)
            return proc.returncode

        print("scoring report: {}".format(report), flush=True)
        score = subprocess.run([sys.executable, str(score_script), str(report)], cwd=str(REPO_ROOT))
        return score.returncode
    finally:
        try:
            runner.destroy_instance(sdb_script, ns.base_dir, REPO_ROOT, check=False)
        except Exception as exc:
            print("failed to destroy instance: {}".format(exc), file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
