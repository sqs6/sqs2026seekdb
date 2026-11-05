#!/usr/bin/env python3
"""Run a subset of mysqltest cases against a locally started seekdb.

复用官方 .github/script/seekdb/mysqltest_for_seekdb.py 的部署与执行逻辑,
只挑选名字前缀匹配的用例, 便于在 CI 里快速回归指定赛题的判分用例.

用法:
    python3 .github/script/seekdb/agent_run_cases.py \
        --seekdb build_release/src/observer/seekdb \
        --obclient deps/3rd/u01/obclient/bin/obclient \
        --mysqltest deps/3rd/u01/obclient/bin/mysqltest \
        --work-dir /tmp/agent_work --port 2881 --case-prefix ai_function.
"""
import argparse
import importlib.util
import os
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
    parser.add_argument("--mysqltest", required=True)
    parser.add_argument("--work-dir", default="/tmp/agent_work")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--case-prefix", default="ai_function.")
    args = parser.parse_args()

    runner = load_runner()
    args.base_dir = os.path.join(args.work_dir, "instance")
    args.work_dir = args.work_dir
    args.seekdb = Path(args.seekdb).resolve()
    args.obclient = Path(args.obclient).resolve()
    args.mysqltest = Path(args.mysqltest).resolve()

    deploy_dir = REPO_ROOT / "tools" / "deploy"
    sdb_script = RUNNER_PATH.with_name("sdb.py")

    cases = [c for c in runner.discover_cases(REPO_ROOT) if c.name.startswith(args.case_prefix)]
    if not cases:
        print("no case matched prefix {}".format(args.case_prefix), file=sys.stderr)
        return 2
    print("selected cases: {}".format(", ".join(c.name for c in cases)), flush=True)

    work_dir = Path(args.work_dir)
    tmp_dir = work_dir / "tmp"
    log_dir = work_dir / "mysqltest_log"
    tmp_dir.mkdir(parents=True, exist_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)

    failed = []
    try:
        runner.prepare_instance(args, REPO_ROOT, sdb_script, deploy_dir)
        for case in cases:
            code, output = runner.run_case(args, deploy_dir, case, tmp_dir, log_dir)
            if code != 0:
                failed.append(case.name)
    finally:
        # 实例销毁前先把 observer 日志抓出来, 便于定位表达式求值失败原因
        try:
            runner.copy_instance_diagnostics(Path(args.base_dir), work_dir / "diag")
            print("instance diagnostics saved to {}".format(work_dir / "diag"), flush=True)
        except Exception as exc:
            print("failed to copy instance diagnostics: {}".format(exc), flush=True)
        runner.destroy_instance(sdb_script, args.base_dir, REPO_ROOT, check=False)

    if failed:
        print("FAILED cases: {}".format(", ".join(failed)), flush=True)
        return 1
    print("all {} cases passed".format(len(cases)), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
