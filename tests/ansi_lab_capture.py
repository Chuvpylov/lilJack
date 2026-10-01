#!/usr/bin/env python3
"""Capture only newly created lab windows on X11; never attach to user panes."""
import argparse
import json
import os,shutil
from pathlib import Path
import subprocess
import time
from PIL import ImageGrab
from ansi_lab import build, ROOT
from x11_window_capture import capture as window_capture

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--terminal',choices=['both','all','owkterm','wezterm','kitty'],default='both',help='both = owkterm+wezterm (sixel); all adds kitty (no sixel: the cell fallback + DECSET 2026 reference)')
    parser.add_argument('--size',default='',help='single WIDTHxHEIGHT (default three sizes)')
    parser.add_argument('--extra',action='append',default=[],help='lab isolation flag, e.g. --extra=--no-ring')
    parser.add_argument('--composite',action='store_true',help='read only the owned window backing pixmap; avoids screen occlusion')
    parser.add_argument('--lab-binary',type=Path,help='use an existing lab binary for a baseline comparison')
    args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    lab=args.lab_binary.resolve() if args.lab_binary else build()
    owk=subprocess.check_output(['bash',str(ROOT.parent/'owkTerm/build.sh')],text=True).strip()
    env=dict(os.environ,DISPLAY=':0',SDL_VIDEODRIVER='x11',OWKTERM_TRACE='1')
    sizes=[tuple(map(int,args.size.split('x')))] if args.size else [(800,560),(1280,720),(1920,1080)]
    records=[]
    terminals=['owkterm','wezterm'] if args.terminal=='both' else ['owkterm','wezterm','kitty'] if args.terminal=='all' else [args.terminal]
    if 'kitty' in terminals and not shutil.which('kitty'):raise SystemExit('kitty is not installed (sudo pacman -S --needed kitty)')
    def xd(*cmd):
        return subprocess.run(['xdotool',*map(str,cmd)],env=env,capture_output=True,text=True)
    for term in terminals:
        for w,h in sizes:
            tag=f'{term}-{w}x{h}';unique=f'codex-ansi-lab-{os.getpid()}-{tag}'
            log=out/f'{tag}.log';childlog=out/f'{tag}-child.log'
            childlog.unlink(missing_ok=True)
            # Child starts after the WM resize; the sleep affects our own process only.
            wrapper=out/f'{tag}-run.py'
            wrapper.write_text('import os,time\ntime.sleep(2)\nos.dup2(os.open('+repr(str(childlog))+',os.O_WRONLY|os.O_CREAT|os.O_TRUNC,0o600),2)\nos.execv('+repr(str(lab))+','+repr([str(lab),'--seconds','25',*args.extra])+')\n')
            if term=='owkterm':
                command=[owk,'--title',unique,'--width',str(w),'--height',str(h),'--seconds','30','--','python3',str(wrapper)]
                search=['--name',unique]
            elif term=='kitty':
                # kitty refuses sixel by design (own graphics protocol): this is the CELL fallback
                # reference, and it honours synchronized output (DECSET 2026). Same font/size, no
                # padding, one process per window, no close confirmation.
                command=['kitty','--config','NONE','--class',unique,'--override','font_family=JetBrains Mono','--override','font_size=14',
                         '--override','window_padding_width=0','--override','confirm_os_window_close=0','--override','initial_window_width=80c','--override','initial_window_height=28c',
                         '--','python3',str(wrapper)]
                search=['--class',unique]
            else:
                config=out/f'{tag}.lua'
                config.write_text("local w=require 'wezterm'\nreturn {enable_tab_bar=false,check_for_updates=false,window_decorations='RESIZE',initial_cols=80,initial_rows=28,window_padding={left=0,right=0,top=0,bottom=0},exit_behavior='Close',font=w.font('JetBrains Mono'),font_size=14}\n")
                command=['wezterm','--config-file',str(config),'start','--always-new-process','--no-auto-connect','--class',unique,'--','python3',str(wrapper)]
                search=['--class',unique]
            record={'terminal':term,'requested':[w,h],'lab':str(lab),'owkterm':owk,'extra':args.extra,'captures':[]}
            with log.open('w') as stream:
                proc=subprocess.Popen(command,env=env,stdout=stream,stderr=stream,start_new_session=True)
                try:
                    deadline=time.monotonic()+12;win=None
                    while time.monotonic()<deadline and proc.poll() is None:
                        found=xd('search','--onlyvisible',*search).stdout.split()
                        if found:win=found[-1];break
                        time.sleep(.1)
                    if not win:raise RuntimeError(f'no owned {term} window: {log}')
                    xd('windowmove',win,40,40);xd('windowsize',win,w,h);xd('windowraise',win)
                    # Measure from the presenter's own startup log, after its terminal queries.
                    deadline=time.monotonic()+10
                    while time.monotonic()<deadline:
                        if childlog.exists() and 'ansi-lab cols=' in childlog.read_text():break
                        time.sleep(.05)
                    else:raise RuntimeError(f'lab did not start: {childlog}')
                    start=time.monotonic()
                    for seconds in (2,20):
                        while time.monotonic()-start<seconds:time.sleep(min(.1,seconds-(time.monotonic()-start)) if seconds>time.monotonic()-start else .001)
                        if not args.composite:xd('windowraise',win)
                        time.sleep(.1)
                        result=xd('getwindowgeometry','--shell',win)
                        if result.returncode:
                            raise RuntimeError(f'owned window {win} disappeared (process={proc.poll()}): {result.stderr}')
                        geometry=dict(line.split('=',1) for line in result.stdout.splitlines())
                        x,y,gw,gh=[int(geometry[k]) for k in ('X','Y','WIDTH','HEIGHT')]
                        path=out/f'{tag}-{seconds}s.png'
                        if args.composite:window_capture(win,gw,gh).save(path)
                        else:ImageGrab.grab(bbox=(x,y,x+gw,y+gh),xdisplay=':0').save(path)
                        record['capture_method']='XComposite backing pixmap' if args.composite else 'ImageGrab screen bounds'
                        record['captures'].append({'seconds':round(time.monotonic()-start,3),'geometry':geometry,'path':str(path)})
                    record['child_log']=childlog.read_text()
                finally:
                    if proc.poll() is None:
                        proc.terminate()
                        try:proc.wait(timeout=3)
                        except subprocess.TimeoutExpired:proc.kill();proc.wait()
            records.append(record);(out/'captures.json').write_text(json.dumps(records,indent=2));print(tag,flush=True)

if __name__=='__main__':main()
