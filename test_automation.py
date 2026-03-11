#!/usr/bin/env python3
"""
Geyser Model Checker Test Automation Script

This script automatically discovers and tests all AIGER (.aig) test cases
in the test/ directory using the Geyser model checker.

Result Types:
  - CORRECT: Property holds for all reachable states
  - INCORRECT: Property is violated (counterexample found)
  - TIMEOUT: Test exceeded the timeout limit
  - ERROR: Solver encountered an error

Usage:
    python3 test_automation.py [options]

Options:
    --timeout TIMEOUT    Timeout in seconds for each test case (default: 60)
    --output FILE        Output report file (default: test_report.txt)
    --engine ENGINE      Geyser engine to use (pdr|car|icar|bmc|bcar, default: pdr)
    --solver PATH        Path to Geyser binary (default: geyser/build/run-geyser)
    --verbose            Enable verbose output
    --help               Show this help message

Author: GitHub Copilot
Date: 2026-03-11
"""

import os
import sys
import glob
import subprocess
import time
import argparse
from pathlib import Path
from typing import List


class TestResult:
    """Represents the result of a single test case"""
    
    def __init__(self, file_path: str):
        self.file_path = file_path
        self.result = "UNKNOWN"
        self.runtime = 0.0
        self.error = None
        self.timeout = False

    def __str__(self):
        status = f"{self.result}"
        if self.timeout:
            status += " (TIMEOUT)"
        elif self.error:
            status += f" (ERROR: {self.error})"
        return f"{self.file_path}: {status} ({self.runtime:.2f}s)"


class GeyserTester:
    """Main test automation class for Geyser model checker"""
    
    def __init__(self, solver_path=None, timeout=60, engine="pdr", verbose=False):
        self.solver_path = solver_path or self._find_solver()
        self.timeout = timeout
        self.engine = engine
        self.verbose = verbose
        self.test_results = []

        if not self.solver_path or not os.path.exists(self.solver_path):
            raise FileNotFoundError(f"Geyser solver not found at {self.solver_path}")

        if self.verbose:
            print(f"Using solver: {self.solver_path}")
            print(f"Engine: {self.engine}")
            print(f"Timeout: {self.timeout}s per test case")

    def _find_solver(self):
        """Auto-detect Geyser run-geyser binary"""
        candidates = [
            "geyser/build/run-geyser",
            "geyser-main/build/run-geyser",
            "./geyser/build/run-geyser",
            "run-geyser"
        ]

        for candidate in candidates:
            if os.path.exists(candidate):
                return candidate

        # Check if it's in PATH
        try:
            result = subprocess.run(
                ["which", "run-geyser"],
                capture_output=True,
                text=True,
                timeout=5
            )
            if result.returncode == 0:
                return result.stdout.strip()
        except Exception:
            pass

        return None

    def find_test_files(self):
        """Find all .aig files in the test directory"""
        test_dir = Path("test")
        if not test_dir.exists():
            raise FileNotFoundError("test/ directory not found")

        aig_files = []
        
        # Recursively find all .aig files
        for pattern in ["**/*.aig", "*.aig"]:
            aig_files.extend(glob.glob(str(test_dir / pattern), recursive=True))

        # Also check root directory for any .aig files
        root_aig_files = glob.glob("*.aig")
        aig_files.extend(root_aig_files)

        # Remove duplicates and sort
        aig_files = sorted(list(set(aig_files)))

        if self.verbose:
            print(f"Found {len(aig_files)} test files")

        return aig_files

    def run_single_test(self, aig_file):
        """Run Geyser on a single AIG file"""
        result = TestResult(aig_file)

        try:
            start_time = time.time()

            # Run Geyser with the specified engine
            cmd = [self.solver_path, f"-e={self.engine}", aig_file]
            process = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=self.timeout
            )

            result.runtime = time.time() - start_time

            # Parse output to determine result
            stdout = process.stdout
            stderr = process.stderr
            output = stdout + stderr

            if process.returncode == 0:
                # Geyser outputs counterexample data if property is INCORRECT
                # If no counterexample, property is CORRECT
                # Counterexample format: digit line, then 'b' lines, then bit vectors, ends with '.'
                
                # Filter out DEBUG lines
                non_debug_lines = [line for line in stdout.split('\n') if not line.startswith('DEBUG:')]
                non_debug_output = '\n'.join(non_debug_lines).strip()
                
                # Check if counterexample is present (non-DEBUG output with structure)
                # Counterexample starts with a number (state index) followed by 'b' lines
                has_counterexample = False
                if non_debug_output:
                    lines = non_debug_output.split('\n')
                    # Check for counterexample pattern: has a digit line and 'b' prefixed lines
                    for i, line in enumerate(lines):
                        if line and (line[0].isdigit() or line.startswith('b')):
                            has_counterexample = True
                            break
                
                if has_counterexample:
                    result.result = "INCORRECT"
                else:
                    result.result = "CORRECT"
            else:
                # Non-zero exit code - capture more detailed error info
                error_msg = f"Exit code {process.returncode}"
                if stderr.strip():
                    error_msg += f" | stderr: {stderr[:100]}"
                result.error = error_msg
                result.result = "ERROR"
                
                if "timeout" in output.lower():
                    result.timeout = True

        except subprocess.TimeoutExpired:
            result.runtime = self.timeout
            result.timeout = True
            result.result = "TIMEOUT"
        except Exception as e:
            result.error = str(e)
            result.result = "ERROR"

        return result

    def run_all_tests(self):
        """Run tests on all AIG files"""
        test_files = self.find_test_files()

        if not test_files:
            print("No .aig test files found!")
            return

        print(f"Running tests on {len(test_files)} AIG files...")
        print("=" * 60)

        for i, aig_file in enumerate(test_files, 1):
            if self.verbose:
                print(f"[{i}/{len(test_files)}] Testing {aig_file}")
            else:
                print(f"Testing {os.path.basename(aig_file)}... ", end="", flush=True)

            result = self.run_single_test(aig_file)
            self.test_results.append(result)

            if self.verbose:
                print(f"  Result: {result}")
            else:
                status = "✓" if result.result in ["SAT", "UNSAT"] else "✗"
                print(f"{status} {result.result}")

        print("=" * 60)

    def generate_report(self, output_file=None):
        """Generate a detailed test report"""
        total_tests = len(self.test_results)
        correct_count = sum(1 for r in self.test_results if r.result == "CORRECT")
        incorrect_count = sum(1 for r in self.test_results if r.result == "INCORRECT")
        unknown_count = sum(1 for r in self.test_results if r.result == "UNKNOWN")
        timeout_count = sum(1 for r in self.test_results if r.timeout)
        error_count = sum(1 for r in self.test_results if r.error and not r.timeout)

        total_runtime = sum(r.runtime for r in self.test_results)
        avg_runtime = total_runtime / total_tests if total_tests > 0 else 0

        report = []
        report.append("Geyser Model Checker Test Report")
        report.append("=" * 40)
        report.append(f"Generated: {time.strftime('%Y-%m-%d %H:%M:%S')}")
        report.append(f"Solver: {self.solver_path}")
        report.append(f"Engine: {self.engine}")
        report.append(f"Timeout: {self.timeout}s per test")
        report.append("")

        report.append("SUMMARY:")
        report.append(f"  Total test cases: {total_tests}")
        report.append(f"  CORRECT: {correct_count}")
        report.append(f"  INCORRECT: {incorrect_count}")
        report.append(f"  UNKNOWN: {unknown_count}")
        report.append(f"  Timeouts: {timeout_count}")
        report.append(f"  Errors: {error_count}")
        report.append(f"  Total runtime: {total_runtime:.2f}s")
        report.append(f"  Average runtime: {avg_runtime:.2f}s")
        report.append("")

        if timeout_count > 0 or error_count > 0:
            report.append("ISSUES:")
            for result in self.test_results:
                if result.timeout or (result.error and not result.timeout):
                    report.append(f"  {result}")
            report.append("")

        report.append("DETAILED RESULTS:")
        report.append("-" * 40)
        for result in self.test_results:
            report.append(str(result))

        report_text = "\n".join(report)

        if output_file:
            with open(output_file, 'w') as f:
                f.write(report_text)
            print(f"Report saved to: {output_file}")
            
            # Also generate CSV file for easier analysis
            csv_file = output_file.replace('.txt', '_data.csv')
            with open(csv_file, 'w') as f:
                f.write("file_path,result,runtime,error\n")
                for result in self.test_results:
                    error_msg = result.error.replace(',', ';') if result.error else ""
                    f.write(f'"{result.file_path}",{result.result},{result.runtime:.2f},"{error_msg}"\n')
            print(f"Data file saved to: {csv_file}")

        return report_text


def main():
    parser = argparse.ArgumentParser(
        description="Geyser Model Checker Test Automation Script",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--timeout", "-t",
        type=int,
        default=60,
        help="Timeout in seconds for each test case (default: 60)"
    )
    parser.add_argument(
        "--output", "-o",
        type=str,
        default="test_report.txt",
        help="Output report file (default: test_report.txt)"
    )
    parser.add_argument(
        "--solver", "-s",
        type=str,
        help="Path to Geyser binary (default: geyser/build/run-geyser)"
    )
    parser.add_argument(
        "--engine", "-e",
        type=str,
        default="pdr",
        choices=["pdr", "car", "icar", "bmc", "bcar"],
        help="Geyser engine to use (default: pdr)"
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Enable verbose output"
    )
    parser.add_argument(
        "--debug-file",
        type=str,
        help="Test a single file and show detailed output/error messages"
    )

    args = parser.parse_args()

    try:
        tester = GeyserTester(
            solver_path=args.solver,
            timeout=args.timeout,
            engine=args.engine,
            verbose=args.verbose
        )

        # Debug mode: test a single file with detailed output
        if args.debug_file:
            print(f"Testing file: {args.debug_file}")
            print("=" * 60)
            
            result = tester.run_single_test(args.debug_file)
            print(f"Result: {result.result}")
            print(f"Runtime: {result.runtime:.2f}s")
            if result.error:
                print(f"Error: {result.error}")
            if result.timeout:
                print("Status: TIMEOUT")
            
            # Run again to capture full output
            print("\nRunning solver directly for detailed output:")
            print("-" * 60)
            cmd = [tester.solver_path, f"-e={tester.engine}", args.debug_file]
            try:
                process = subprocess.run(
                    cmd,
                    timeout=args.timeout,
                    text=True
                )
                print(f"Exit code: {process.returncode}")
            except subprocess.TimeoutExpired:
                print("Solver timed out")
            except Exception as e:
                print(f"Error running solver: {e}")
        else:
            tester.run_all_tests()
            report = tester.generate_report(args.output)

            if not args.verbose:
                print("\n" + report)

    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
