#!/usr/bin/env python3
import subprocess
import os
import argparse

LOG_FILE = "opentitan_test_log_"
SUCCESS_COUNT = 0
FAIL_COUNT = 0
JOBS = 8

black_list = {"//sw/device/tests:aes_prng_reseed_test_sim_verilator", "//sw/device/tests:alert_handler_reverse_ping_in_deep_sleep_test_sim_verilator", 
"//sw/device/tests:aon_timer_sleep_wdog_sleep_pause_test_sim_verilator", "//sw/device/tests:aon_timer_smoketest_sim_verilator"}

def run_cmd(cmd, timeout):
    """Run a shell command with timeout and return (success, output)."""
    try:
        result = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",  # handle invalid UTF-8 
            timeout=timeout
        )
        return result.returncode == 0, result.stdout + result.stderr
    except subprocess.TimeoutExpired as e:
        return False, f"TIMEOUT after {timeout}s\n{e.stdout or ''}{e.stderr or ''}"
    except Exception as e:
        return False, str(e)

def get_test_targets():
    """Get only tests defined directly under //sw/device/tests/..."""
    cmd = ["bazel", "query", "tests(//sw/device/tests/...)"]
    ok, out = run_cmd(cmd, timeout=60)  # short timeout for query
    if not ok:
        print("Error listing tests!")
        print(out)
        exit(1)
    all_tests = out.strip().splitlines()
    # Keep only those in sw/device/tests
    sw_tests = [t for t in all_tests if t.startswith("//sw/device/tests:")]
    return sw_tests

def filter_sw_only_tests(tests):
    """Return only software tests that are top-level and do not depend on hardware."""
    sw_tests = []
    for t in tests:
        # must be in sw/device/tests top-level (no subfolders)
        if not t.startswith("//sw/device/tests:"):
            continue
        # skip tests that depend on simulation or FPGA
        if "_sim_" in t or "_fpga" in t:
            continue
        if "test" not in t.lower():
            continue
        sw_tests.append(t)
    return sw_tests

def main():
    global SUCCESS_COUNT, FAIL_COUNT

    parser = argparse.ArgumentParser(description="Opentitan Test Executor")
    parser.add_argument(
        "--test", type=str, required=True,
        choices=["dv", "verilator", "fpga"],
        help="Test suite to execute: sw=software-only, verilator=sim tests, smoke=uart smoke"
    )
    parser.add_argument(
        "--timeout", type=int, default=1501,
        help="Timeout (seconds) for each test (default: 1200s)"
    )
    args = parser.parse_args()
    tests = get_test_targets()

    if args.test == "verilator":
        test_list = [t for t in tests if "verilator" in t.lower()]
    elif args.test == "dv":
        test_list = [t for t in tests if "_dv" in t.lower()]
    elif args.test == "fpga":
        test_list = [t for t in tests if "fpga_cw310_test_rom" in t.lower()]
    else:  # smoke
        test_list = [t for t in tests if "smoke" in t.lower() and "uart" in t.lower()]

    print(f"Found {len(tests)} tests: Running {len(test_list)} {args.test} tests")

    with open(f"{LOG_FILE}{args.test}.txt", "w") as log:
        for test in tests:
            log.write(str(test + "\n"))
        for i, test in enumerate(test_list, 1):
            print(f"[{i}/{len(test_list)}] Running {test} ...")
            # if test not in black_list: 
            ok, out = run_cmd(["bazel", "test", test, f"--jobs={JOBS}", "--test_output=errors"], timeout=args.timeout)
            if ok:
                SUCCESS_COUNT += 1
                print(f"PASS {test}")
            else:
                FAIL_COUNT += 1
                print(f"** FAIL {test} (logged)")
                log.write(f"\n=== FAILED: {test} ===\n")
                log.write(out + "\n")

        log.write(f"\nSUMMARY: {SUCCESS_COUNT} passed, {FAIL_COUNT} failed\n")

    print("\n==========================")
    print(f"Finished: {SUCCESS_COUNT} passed, {FAIL_COUNT} failed")
    print(f"Full log written to {LOG_FILE}")

if __name__ == "__main__":
    main()
