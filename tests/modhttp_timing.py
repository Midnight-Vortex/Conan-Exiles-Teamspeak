"""Timing of the game mod's position requests, read from a ConanSandbox.log (doku 106).

The game writes two lines per request, both on the GAME thread:
  [time][frame]LogModHttp: Starting request from mod 'teamspeak' with id X and a 2.0s timeout to: <url>
  [time][frame]LogModHttp: Request X succeeded with code 200 and content length N.
The [frame] number is the game's frame counter (0..999). The answer line is written when the game thread
handles the answer, NOT when the plugin sent it. So:
  - answer in a LATER frame  -> the game kept rendering while waiting: it does not wait for the answer;
  - answer in the SAME frame after a long time -> that one frame took that long (the game thread did not
    tick). That is either the game waiting for the answer, or the game busy with something else (the other
    lines in that frame tell which) - with the slow stand-in (tests/slow_reply_standin.ps1) it is the
    first: then the game waits.

Usage:  python modhttp_timing.py <ConanSandbox.log> [from HH:MM] [to HH:MM]   (times in the log's UTC)
"""
import collections
import re
import sys

LINE = re.compile(r"^\[(\d{4})\.(\d\d)\.(\d\d)-(\d\d)\.(\d\d)\.(\d\d):(\d{3})\]\[\s*(\d+)\](.*)$")
START = re.compile(r"LogModHttp: Starting request from mod 'teamspeak' with id (\w+) and a (\S+) timeout to: (\S+)")
DONE = re.compile(r"LogModHttp: (?:Warning: )?Request (\w+) (succeeded|failed)")


def hhmm(s):
    h, m = s.split(":")
    return int(h) * 3600 + int(m) * 60


def clock(t):
    return "%02d:%02d:%06.3f" % (t // 3600, t % 3600 // 60, t % 60)


def main():
    path = sys.argv[1]
    t_from = hhmm(sys.argv[2]) if len(sys.argv) > 2 else None
    t_to = hhmm(sys.argv[3]) if len(sys.argv) > 3 else None
    starts = {}
    pairs = []          # (seconds, start frame, answer frame, start time, url, result, timeout)
    in_flight = 0
    max_in_flight = 0
    other_in_frame = collections.defaultdict(list)   # (frame, start time) -> other lines while a request was open
    open_ids = {}
    for raw in open(path, encoding="utf-8", errors="replace"):
        m = LINE.match(raw)
        if not m:
            continue
        t = int(m.group(4)) * 3600 + int(m.group(5)) * 60 + int(m.group(6)) + int(m.group(7)) / 1000.0
        if (t_from is not None and t < t_from) or (t_to is not None and t > t_to):
            continue
        frame, rest = int(m.group(8)), m.group(9)
        a = START.search(rest)
        if a:
            starts[a.group(1)] = (t, frame, a.group(3), a.group(2))
            open_ids[a.group(1)] = []
            in_flight += 1
            max_in_flight = max(max_in_flight, in_flight)
            continue
        b = DONE.search(rest)
        if b and b.group(1) in starts:
            t0, f0, url, tmo = starts.pop(b.group(1))
            others = open_ids.pop(b.group(1), [])
            in_flight -= 1
            pairs.append((t - t0, f0, frame, t0, url, b.group(2), tmo, others))
            continue
        if "LogModHttp" not in rest:
            for k in open_ids:
                if len(open_ids[k]) < 4:
                    open_ids[k].append(rest.strip()[:110])

    print("requests: %d answered, %d still open at the end of the window" % (len(pairs), len(starts)))
    if not pairs:
        return
    print("timeouts used: %s" % dict(collections.Counter((p[4].split("/")[-1], p[6]) for p in pairs)))
    print("most requests open at the same time: %d" % max_in_flight)
    d = sorted(p[0] for p in pairs)
    q = lambda f: d[min(len(d) - 1, int(f * len(d)))] * 1000
    print("time until the game handled the answer, ms: median %.0f  p90 %.0f  p99 %.0f  max %.0f" % (q(.5), q(.9), q(.99), d[-1] * 1000))
    fails = sum(1 for p in pairs if p[5] == "failed")
    if fails:
        print("failed requests: %d" % fails)
    slow = [p for p in pairs if p[0] >= 0.15]
    same = [p for p in slow if p[1] == p[2]]
    later = [p for p in slow if p[1] != p[2]]
    print("answers after 150 ms or more: %d  -> handled in the SAME frame: %d, in a LATER frame: %d" % (len(slow), len(same), len(later)))
    for p in sorted(slow, key=lambda p: -p[0])[:12]:
        gap = (p[2] - p[1]) % 1000
        print("   %5.0f ms  %s UTC  frame %3d -> %3d (%s)%s" % (
            p[0] * 1000, clock(p[3]), p[1], p[2], "same frame" if gap == 0 else "+%d frames" % gap,
            ("   meanwhile: " + " | ".join(p[7][:2])) if p[7] else ""))
    if slow:
        if len(later) > len(same):
            print("VERDICT: the game kept rendering while answers were late -> it does NOT wait for the answer.")
        else:
            print("VERDICT: late answers were handled in the same frame -> that frame lasted as long as the wait."
                  " With the slow stand-in this means the game WAITS; in a normal log look at 'meanwhile'.")


if __name__ == "__main__":
    main()
