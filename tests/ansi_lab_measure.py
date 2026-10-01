#!/usr/bin/env python3
"""Measure the captured lab's edge bands and graph columns (no image edits)."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import numpy as np
from PIL import Image

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('folder',type=Path);args=p.parse_args()
    records=json.loads((args.folder/'captures.json').read_text());out=[]
    for record in records:
        cols,rows,cw,ch=map(int,re.search(r'cols=(\d+) rows=(\d+) cell=(\d+)x(\d+)',record['child_log']).groups())
        for capture in record['captures']:
            a=np.asarray(Image.open(capture['path']).convert('RGB')).astype(int)
            # Infer canvas origin from the largest run of known graph background.
            graphbg=(np.abs(a-np.array([16,32,48])).max(axis=2)<=4)
            ys,xs=np.where(graphbg)
            if not len(xs):out.append({'path':capture['path'],'error':'no graph background'});continue
            ox=int(xs.min())-cw;oy=int(ys.min())-ch
            green=(a[:,:,1]>140)&(a[:,:,0]<110)&(a[:,:,2]<170)
            gw=(cols//4-2)*cw
            gaps=[]
            for k in range(4):
                x=ox+(k*(cols//4)+1)*cw;y=oy+ch
                region=green[y:y+2*ch,x:x+gw]
                gaps.append(int((~region.any(axis=0)).sum()))
            # Colors are sixel-quantized, hence the small tolerance.
            gold=(np.abs(a-np.array([255,207,64])).max(axis=2)<=5)
            blue=(np.abs(a-np.array([78,183,255])).max(axis=2)<=5)
            mask=gold|blue
            left=ox+2*cw-1;right=ox+(cols//2-2)*cw
            top=oy+5*ch-1;bottom=oy+(rows-2)*ch
            edges={}
            for name,region in {
                'top':mask[top-3:top+4,left+5:right-5],
                'bottom':mask[bottom-3:bottom+4,left+5:right-5],
                'left':mask[top+5:bottom-5,left-3:left+4],
                'right':mask[top+5:bottom-5,right-3:right+4],
            }.items():
                sums=region.sum(axis=1 if name in ('top','bottom') else 0)
                edges[name]={'lit_band_pixels':int((sums>0).sum()),'counts':sums.tolist()}
            # Run widths across the top edge, excluding corners and any seam.
            strip=mask[top-3:top+4,left+5:right-5].any(axis=0)
            runs=[];n=0
            for bit in [*strip,False]:
                if bit:n+=1
                elif n:runs.append(n);n=0
            top_colors=['G' if gold[top-1:top+2,x].any() else 'B' if blue[top-1:top+2,x].any() else '.' for x in range(left+5,right-5)]
            out.append({'terminal':record['terminal'],'requested':record['requested'],'path':capture['path'],
                'capture_seconds':capture['seconds'],'cell':[cw,ch],'origin':[ox,oy],
                'graph_missing_columns':gaps,'ring_edges':edges,'top_dot_run_widths':dict(Counter(runs)),
                'top_color_order_sample':''.join(top_colors[:96])})
    (args.folder/'measurements.json').write_text(json.dumps(out,indent=2))
    for row in out:print(json.dumps(row))
if __name__=='__main__':main()
