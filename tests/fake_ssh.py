#!/usr/bin/env python3
"""A stand-in for `ssh` in the loader tests: runs the remote command on this computer instead.

    fake_ssh.py [ssh options...] user@host 'remote command'

The command runs under $FAKE_MPC_SHELL (default /bin/sh) with $FAKE_MPC_PATH as its PATH when that is
set (the tests use a folder of busybox applets there, to check the commands work on a small shell).
Hosts named unreachable.invalid, refused.invalid and denied.invalid answer as ssh does when that
goes wrong. Every call is appended to $FAKE_MPC_LOG when that is set.
"""
import os
import subprocess
import sys

args = sys.argv[1:]
if len(args) < 2:
    sys.exit("fake_ssh: need user@host and a command")
command, dest = args[-1], args[-2]
host = dest.split("@", 1)[-1]
log = os.environ.get("FAKE_MPC_LOG")
if log:
    with open(log, "a", encoding="utf-8") as f:
        f.write("%s\t%s\n" % (dest, command.replace("\n", "\\n")))
if host == "unreachable.invalid":
    sys.stderr.write("ssh: connect to host %s port 22: Operation timed out\n" % host)
    sys.exit(255)
if host == "refused.invalid":
    sys.stderr.write("ssh: connect to host %s port 22: Connection refused\n" % host)
    sys.exit(255)
if host == "denied.invalid":
    sys.stderr.write("root@%s: Permission denied (publickey,password).\n" % host)
    sys.exit(255)
env = dict(os.environ)
if os.environ.get("FAKE_MPC_PATH"):
    env["PATH"] = os.environ["FAKE_MPC_PATH"]
shell = os.environ.get("FAKE_MPC_SHELL", "/bin/sh").split()
p = subprocess.run(shell + ["-c", command], env=env, cwd=os.environ.get("FAKE_MPC_HOME", "/"))
sys.exit(p.returncode)
