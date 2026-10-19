---
name: Bug report
about: Report something that doesn't work as expected (please attach the log)
title: "[BUG]"
labels: bug
assignees: ''
type: Bug

---

**Describe the bug**
A clear and concise description of what the bug is.

**To Reproduce**
Describe precisely how to reproduce the bug.

**Expected behavior**
A clear and concise description of what you expected to happen.

**Log**
Please attach the log file:
1. Go to Settings --> System --> About and tap "Build number" five times, until a pop-up says developer options are turned on.
2. Go to Settings --> Developer options and enable "Log to microSD".
3. Reproduce the issue.
4. Attach `.local/sonix_player.log` from the root of the microSD card (`.local` is a hidden folder).

If the issue happens during a library scan, also enable "Database log" before reproducing it.
If it is a Bluetooth problem, also enable "Bluetooth log" and attach `.local/bluetooth.log`.
If it is a crash, a reboot or a freeze, attach the log as it is **after** the device has restarted: every start writes what the kernel kept of the previous run into the top of the new log.

More, including how to take a full diagnostic bundle over ADB (`sonix-player/tools/sonix-bugreport.sh`) and which log belongs to which symptom: [docs/collecting-logs.md](../../docs/collecting-logs.md).

**Screenshots**
If applicable, add screenshots to help explain your problem.

**Sonix Player info**
 - Operating system version: [e.g. 1.1.0]
 - Device: [TempoTec Variations V1]

**Additional context**
Add any other context about the problem here.
