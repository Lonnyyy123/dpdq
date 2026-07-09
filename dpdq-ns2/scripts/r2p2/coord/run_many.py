#!/usr/bin/env python3
import argparse
import os
import subprocess
import sys
import re
import time
from datetime import datetime

# Default options to be passed to the 'run' executable.
DEFAULT_RUN_OPTS = ["1", "1", "1", "1", "0"]

def get_required_threads(sh_filepath):
    """
    Reads the given .sh file and attempts to extract max_threads.
    The line should look like: max_threads='8' or max_threads="8".
    Raises an Exception if no max_threads definition is found.
    """
    thread_pattern = re.compile(r"max_threads\s*=\s*['\"](\d+)['\"]")
    with open(sh_filepath, "r") as f:
        for line in f:
            m = thread_pattern.search(line)
            if m:
                return int(m.group(1))
    raise Exception(f"max_threads not set in {sh_filepath}")

# Examples 
# python3 run_many.py /home/prasopou/nwsim/scripts/r2p2/coord/config/thesis/sender-algos/AddIncr/ --threads 48
# nohup python3 run_many.py /home/prasopou/nwsim/scripts/r2p2/coord/config/thesis/sender-algos/AddIncr/ --threads 48 &
# python3 run_many.py /home/prasopou/nwsim/scripts/r2p2/coord/config/thesis/sender-algos/AddIncr/ -r --threads 48
def main():
    parser = argparse.ArgumentParser(
        description="Calls 'run' from this directory for each .sh config file in the provided directory, scheduling them in parallel based on thread requirements."
    )
    parser.add_argument("directory", help="Directory containing .sh config files")
    parser.add_argument(
        "-r", "--recursive", action="store_true", help="Recursively search for .sh files in subdirectories"
    )
    parser.add_argument(
        "--threads",
        type=int,
        default=1,
        help="Total number of threads available for all jobs"
    )
    args = parser.parse_args()

    total_threads = args.threads

    # Check that the current working directory is the same as the directory where this script resides.
    script_dir = os.path.dirname(os.path.abspath(__file__))
    current_dir = os.getcwd()
    if script_dir != current_dir:
        print(f"Error: Please run the script from its directory: {script_dir}")
        sys.exit(1)

    # Verify that the provided directory exists and is a directory.
    if not os.path.isdir(args.directory):
        print(f"Error: Provided directory does not exist or is not a directory: {args.directory}")
        sys.exit(1)

    # Collect all .sh files.
    sh_files = []
    if args.recursive:
        for root, _, files in os.walk(args.directory):
            for file in files:
                if file.endswith(".sh"):
                    sh_files.append(os.path.join(root, file))
    else:
        for file in os.listdir(args.directory):
            if file.endswith(".sh"):
                sh_files.append(os.path.join(args.directory, file))

    if not sh_files:
        print(f"No .sh config files found in {args.directory}")
        sys.exit(0)

    # Create the logs directory structure.
    logs_dir = os.path.join(script_dir, "run_many_logs")
    os.makedirs(logs_dir, exist_ok=True)
    run_subdir_name = f"{os.path.basename(os.path.abspath(args.directory))}_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
    run_subdir = os.path.join(logs_dir, run_subdir_name)
    os.makedirs(run_subdir)

    # Determine the path to the 'run' executable in the same directory as this script.
    run_executable = os.path.join(script_dir, "run")
    if not os.path.isfile(run_executable):
        print(f"Error: 'run' executable not found in {script_dir}")
        sys.exit(1)
    if not os.access(run_executable, os.X_OK):
        print("Error: 'run' is not executable. Please check permissions.")
        sys.exit(1)

    # Build the pending_jobs list.
    # Each entry will be a dict with keys: 'sh_file' and 'required_threads'
    pending_jobs = []
    for sh_file in sh_files:
        try:
            req = get_required_threads(sh_file)
            if req > total_threads:
                raise Exception(f"Job {sh_file} requires {req} threads, which is more than the total available ({total_threads})")
            pending_jobs.append({
                "sh_file": sh_file,
                "required_threads": req
            })
        except Exception as e:
            print(f"Error processing {sh_file}: {e}")
            sys.exit(1)

    # Dictionaries to store process exit codes by file.
    exit_codes = {}

    # List to track running jobs.
    # Each job in running_jobs is a dict with keys: 'sh_file', 'required_threads', 'process', 'log_file', 'log_filepath'
    running_jobs = []

    print(f"Starting jobs with a total thread capacity of {total_threads}...")
    # Main loop continues until no pending jobs remain and all running jobs are done.
    while pending_jobs or running_jobs:
        # Check running jobs; if finished, free up threads.
        finished_jobs = []
        for job in running_jobs:
            proc = job["process"]
            # poll() returns None if process is still running.
            if proc.poll() is not None:
                exit_codes[job["sh_file"]] = proc.returncode
                # Close the log file.
                job["log_file"].close()
                finished_jobs.append(job)
                print(f"Finished {job['sh_file']} with exit code {proc.returncode}")

        # Remove finished jobs from running_jobs.
        for job in finished_jobs:
            print(f"Removing {job['sh_file']} from running jobs")
            running_jobs.remove(job)

        # Compute available threads.
        used_threads = sum(job["required_threads"] for job in running_jobs)
        available = total_threads - used_threads

        # Try to launch any pending jobs that fit in the available capacity.
        # Work on a copy because we may remove elements from pending_jobs.
        pending_jobs_copy = pending_jobs[:]
        for job in pending_jobs_copy:
            if job["required_threads"] <= available:
                sh_file = job["sh_file"]
                log_filename = os.path.basename(sh_file) + ".txt"
                log_filepath = os.path.join(run_subdir, log_filename)
                log_file = open(log_filepath, "w")
                cmd = [run_executable, sh_file] + DEFAULT_RUN_OPTS
                print(f"Running command: {cmd}")
                proc = subprocess.Popen(cmd, stdout=log_file, stderr=subprocess.STDOUT)
                print(f"Adding {sh_file} to running jobs. There are {len(pending_jobs)-1} jobs remaining")

                running_jobs.append({
                    "sh_file": sh_file,
                    "required_threads": job["required_threads"],
                    "process": proc,
                    "log_file": log_file,
                    "log_filepath": log_filepath
                })
                pending_jobs.remove(job)
                # Update available threads.
                available -= job["required_threads"]

        # Sleep briefly before next check.
        time.sleep(5)

    # Write a main log summarizing all exit codes.
    main_log_path = os.path.join(run_subdir, "main_log.txt")
    with open(main_log_path, "w") as main_log:
        main_log.write("Run Summary:\n")
        for file, code in exit_codes.items():
            main_log.write(f"{file}: Exit code {code}\n")
        if any(code != 0 for code in exit_codes.values()):
            main_log.write("\nOne or more commands failed.\n")
        else:
            main_log.write("\nAll commands executed successfully.\n")

    print(f"Logs and summary written to {run_subdir}")

if __name__ == "__main__":
    main()
