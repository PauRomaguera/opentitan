#!/usr/bin/env python3
import subprocess
import argparse

LOG_FILE = "opentitan_test_log_"
SUCCESS_COUNT = 0
FAIL_COUNT = 0
JOBS = 8

black_list = {
    "//sw/device/tests:aes_prng_reseed_test_sim_verilator",
    "//sw/device/tests:alert_handler_reverse_ping_in_deep_sleep_test_sim_verilator",
    "//sw/device/tests:aon_timer_sleep_wdog_sleep_pause_test_sim_verilator",
    "//sw/device/tests:aon_timer_smoketest_sim_verilator",
}

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
            timeout=timeout,
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
    return [t for t in all_tests if t.startswith("//sw/device/tests:")]

def main():
    global SUCCESS_COUNT, FAIL_COUNT

    parser = argparse.ArgumentParser(description="Opentitan Test Executor")
    parser.add_argument(
        "--test",
        type=str,
        required=True,
        choices=["dv", "verilator", "fpga"],
        help="Test suite to execute",
    )
    parser.add_argument(
        "--timeout", type=int, default=1501,
        help="Timeout (seconds) for each test (default: 1501s)",
    )
    args = parser.parse_args()
    tests = get_test_targets()

    if args.test == "verilator":
        test_list = [t for t in tests if "verilator" in t.lower()]
    elif args.test == "dv":
        test_list = [t for t in tests if "_dv" in t.lower()]
    elif args.test == "fpga":
        test_list = [t for t in tests if "fpga_cw310_test_rom" in t.lower()]
    else:  # fallback
        test_list = []

    print(f"Found {len(tests)} tests: Running {len(test_list)} {args.test} tests")

    log_path = f"{LOG_FILE}{args.test}.txt"
    with open(log_path, "w") as log:
        for i, test in enumerate(test_list, 1):
            print(f"[{i}/{len(test_list)}] Running {test} ...")
            ok, out = run_cmd(
                ["bazel", "test", test, f"--jobs={JOBS}", "--test_output=errors"],
                timeout=args.timeout,
            )
            if ok:
                SUCCESS_COUNT += 1
                print(f"PASS {test}")
                log.write(f"\n=== PASSED: {test} ===\n")
            else:
                FAIL_COUNT += 1
                print(f"** FAIL {test}")
                log.write(f"\n=== FAILED: {test} ===\n")
            log.write(out + "\n")

        log.write(f"\nSUMMARY: {SUCCESS_COUNT} passed, {FAIL_COUNT} failed\n")

    print("\n==========================")
    print(f"Finished: {SUCCESS_COUNT} passed, {FAIL_COUNT} failed")
    print(f"Full log written to {log_path}")

if __name__ == "__main__":
    main()
