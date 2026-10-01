#!/usr/bin/env python3
"""Compare a lab screenshot with known synthetic frames; detect a shared row split.
Read-only pixel analysis. A mixed frame does not identify display vs readback tearing.
"""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from PIL import Image

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('image',type=Path);p.add_argument('--cols',type=int,default=75)
    p.add_argument('--cell',default='17x39');p.add_argument('--origin',default='0,0')
    p.add_argument('--frames',type=int,default=60)
    args=p.parse_args();cw,ch=map(int,args.cell.split('x'));ox,oy=map(int,args.origin.split(','))
    actual=np.asarray(Image.open(args.image).convert('RGB'))
    sw=(args.cols//4-2)*10;sh=40;tw=sw//10*cw;th=2*ch
    x=np.arange(sw,dtype=np.float32);tx=np.arange(tw)[None,:]*sw//tw;ty=np.arange(th)[:,None]*sh//th
    # Encoder percentage palette, then WezTerm's integer percentage conversion.
    bg=np.array([15,30,45],dtype=np.uint8);ink=np.array([63,221,127],dtype=np.uint8)
    errors=np.zeros((args.frames,th),dtype=np.int64)
    regions=[]
    for k in range(4):
        left=ox+(k*(args.cols//4)+1)*cw;top=oy+ch
        regions.append(actual[top:top+th,left:left+tw])
    for f in range(args.frames):
        for k,region in enumerate(regions):
            v=.50+.32*np.sin((x+f)*.11+k)+.08*np.sin((x+f)*.37)
            y=sh-2-(v*(sh-4)).astype(int)
            source=np.zeros((sh,sw),dtype=bool);source[y,np.arange(sw)]=True;source[y+1,np.arange(sw)]=True
            expected=np.where(source[ty,tx][:,:,None],ink,bg)
            errors[f]+=(region!=expected).any(axis=2).sum(axis=1)
    single=int(np.argmin(errors.sum(axis=1)))
    candidates=[]
    for f in range(args.frames-1):
        for cut in range(1,th):
            candidates.append((int(errors[f,:cut].sum()+errors[f+1,cut:].sum()),f,cut))
    error,frame,cut=min(candidates)
    result={'image':str(args.image.resolve()),'sha256':hashlib.sha256(args.image.read_bytes()).hexdigest(),
            'compared_graph_pixels':4*tw*th,'single_frame':single,'single_frame_mismatched_pixels':int(errors[single].sum()),
            'split_frames':[frame,frame+1],'split_local_y':cut,'split_screen_y':oy+ch+cut,
            'split_mismatched_pixels':error,'interpretation':'exact adjacent-frame row split' if error==0 else 'no exact adjacent-frame row split',
            'limit':'Cannot distinguish displayed tearing from asynchronous screenshot readback.'}
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
