"""The visual and speed baseline: every campaign mission played the same way, its window saved.

usage: python3 baseline.py --exe build/tnfree.exe --game DIR --cd DIR --user DIR --out DIR
                           [--missions 1,2,...] [--size 1280x720] [--no-bench]

Each mission runs in lockstep (port/test/lockstep.c): started straight from the shell, the game's
clock 0x32 ticks a frame, no timer, no sound, no window on the screen (SDL's offscreen driver),
so the same build and settings give the same pictures, pixel for pixel, on every run.  The player
stands where the mission starts and turns: a picture at frame 100 (ahead), 250 (half a turn
later, behind) and 400 (a quarter turn more).  --user is the settings (tnfree.ini) and mods to
draw with; its Show FPS should be off (the meter changes from run to run).

Writes to --out: mNN_fFFFFF.png for every shot, results.json (per mission: its name, the frames,
the time a frame unthrottled, the weather, the exit) and, unless --no-bench, the built-in
benchmark's results (--user's tnfree-bench.txt).  Needs Pillow for the PNGs."""
import argparse, json, os, re, subprocess, sys, time
from PIL import Image

# the campaign's missions in the order they are played (dist/README.txt): number, name, suit
CAMPAIGN = [
    (46, 'XR-5 Qualification', 'Standard'), (47, 'Combat Certification', 'Standard'),
    (37, 'Operation Cannonball', 'Standard'), (38, 'Operation Panama', 'Standard'),
    (1, 'Operation Eagle Eye', 'Scout'), (2, 'Operation Sherwood', 'Standard'),
    (3, 'Operation Seagull', 'Standard'), (4, "Operation Eagle's Roost", 'Standard'),
    (5, 'Operation Valkyrie', 'Standard'), (6, "Operation Eagle's Nest", 'Standard'),
    (7, 'Operation Mariner', 'Standard'), (8, 'Operation Periscope', 'Scout'),
    (9, 'Operation Ivanhoe', 'Heavy'), (32, 'Operation Meteor', 'Standard'),
    (33, 'Operation Meteorite', 'Standard'), (10, 'Operation Oilslick', 'Heavy'),
    (11, 'Operation Market Square', 'Standard'), (12, 'Operation Eggshell', 'Standard'),
    (13, 'Operation Conductor', 'Standard'), (34, 'Operation Meteor Shower', 'Heavy'),
    (29, 'Operation Goblin', 'Standard'), (15, 'Operation Undertow', 'Standard'),
    (17, 'Operation Ivy', 'Standard'), (16, 'Operation Seraphim', 'Heavy'),
    (36, 'Operation Cropduster', 'Heavy'), (14, 'Operation Mercury', 'Standard'),
    (20, 'Operation Black Widow', 'Standard'), (39, 'Operation Cold War', 'Pirate'),
    (21, 'Operation Aegis', 'Heavy'), (35, 'Operation Buzzard', 'Heavy'),
    (22, 'Operation Sparkplug', 'Heavy'), (23, 'Operation Cadmium', 'Heavy'),
    (19, 'Operation Bloodline', 'Heavy'), (24, 'Operation Fusebox', 'Heavy'),
    (27, 'Operation Calais', 'Heavy'), (25, 'Operation Pigpen', 'Heavy'),
    (26, 'Operation Hyperbole', 'Heavy'), (30, 'Operation Icepick', 'Heavy'),
    (31, 'Operation Absolute Zero', 'Heavy'),
]
SHOTS = (100, 250, 400)
TURNS = '150:L32768,0;300:L16384,0'    # mouselook turns, angle units (0x10000 a full turn)
WEATHER = {0: 'rain', 1: 'snow', 255: 'none'}


def run(args, env_extra, extra_args=(), timeout=600):
    env = dict(os.environ, SDL_VIDEO_DRIVER='offscreen', SDL_AUDIO_DRIVER='dummy',
               FREEFALL_WINDOW=args.size, **env_extra)
    cmd = [args.exe, '--user', args.user, '--game', args.game, '--cd', args.cd, *extra_args]
    t = time.time()
    try:
        p = subprocess.run(cmd, env=env, cwd=os.path.dirname(os.path.abspath(args.exe)),
                           capture_output=True, text=True, errors='replace', timeout=timeout)
        return p.returncode, p.stdout + p.stderr, time.time() - t
    except subprocess.TimeoutExpired as e:
        return None, (e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or ''), timeout


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--exe', required=True)
    ap.add_argument('--game', required=True)
    ap.add_argument('--cd', required=True)
    ap.add_argument('--user', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--missions', help='comma-separated mission numbers (default: the campaign)')
    ap.add_argument('--size', default='1280x720')
    ap.add_argument('--no-bench', action='store_true')
    args = ap.parse_args()
    args.exe, args.user, args.out = (os.path.abspath(p) for p in (args.exe, args.user, args.out))
    os.makedirs(args.out, exist_ok=True)
    want = [int(m) for m in args.missions.split(',')] if args.missions else None
    missions = [m for m in CAMPAIGN if want is None or m[0] in want]

    results = []
    for no, name, suit in missions:
        prefix = os.path.join(args.out, 'm%02d_' % no)
        code, out, sec = run(args, {'FREEFALL_LOCKSTEP': '-', 'FREEFALL_LOCKSTEP_MISSION': str(no),
                                    'FREEFALL_LOCKSTEP_SHOTS': ','.join(map(str, SHOTS)),
                                    'FREEFALL_LOCKSTEP_INPUT': TURNS, 'FREEFALL_LOCKSTEP_SHOT': prefix})
        r = {'mission': no, 'name': name, 'suit': suit, 'exit': code, 'seconds': round(sec, 1), 'shots': []}
        m = re.search(r'shots done, (\d+) frames in ([\d.]+) s \(([\d.]+) ms a frame\), weather (\d+)', out)
        if m:
            r.update(frames=int(m.group(1)), ms_per_frame=float(m.group(3)),
                     weather=WEATHER.get(int(m.group(4)), m.group(4)))
        else:
            ended = re.search(r'lockstep: the mission ended at frame (\d+)', out)
            r['ended_at'] = int(ended.group(1)) if ended else None
            r['log_tail'] = out.strip().split('\n')[-5:]
        for f in SHOTS:
            bmp = '%sf%05d.bmp' % (prefix, f)
            if os.path.exists(bmp):
                png = bmp[:-4] + '.png'
                Image.open(bmp).convert('RGB').save(png, optimize=True)
                os.remove(bmp)
                r['shots'].append(os.path.basename(png))
        results.append(r)
        print('%2d %-26s %s' % (no, name, ' '.join('%s=%s' % (k, r[k]) for k in
              ('frames', 'ms_per_frame', 'weather', 'ended_at', 'exit') if k in r)), flush=True)
        json.dump(results, open(os.path.join(args.out, 'results.json'), 'w'), indent=1)

    if not args.no_bench:
        code, out, sec = run(args, {}, ('--bench',), timeout=900)
        bench = os.path.join(args.user, 'tnfree-bench.txt')
        if os.path.exists(bench):
            text = open(bench, errors='replace').read()
            last = text[max(text.rfind('Terra Nova: Freefall benchmark'), 0):]     # the newest run's report
            open(os.path.join(args.out, 'bench.txt'), 'w').write(last.strip() + '\n')
            print(last.strip())
        else:
            print('benchmark: no results (exit %s)' % code, file=sys.stderr)


if __name__ == '__main__':
    main()
