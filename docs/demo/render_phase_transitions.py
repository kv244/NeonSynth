#!/usr/bin/env python3
"""Phase Transitions: six-minute, three-generation NeonSynth composition.
SPDX-License-Identifier: AGPL-3.0-only
Uses the latest render_machine_remembers.py beside this file for JUCE state helpers
and native-plugin path conventions; does not modify that file.

python render_phase_transitions.py --out phase_score
python render_phase_transitions.py --vst "C:/Program Files/Common Files/VST3/NeonSynth.vst3" --out phase_transitions
python render_phase_transitions.py --vst /path/to/NeonSynth.vst3 --preview --out phase_preview

Audio dependencies: numpy, mido, pedalboard, compatible NeonSynth v1.11 VST3.
No audio was rendered or auditioned when this script was prepared.
"""
import argparse
import bisect
import json
import math
import random
import runpy
import struct
import sys
import wave
from pathlib import Path

SR = 48000
BPM = 120
BEAT = .5
STAGE = 120.
TAIL = 8.
TOTAL = 368.
SEED = 244

# Decision: deterministic composition, not uncontrolled randomness. Seeded scheduling
# makes the score reproducible; native granular randomness can still vary by build.
def make_score(stage_seconds=STAGE):
    rng = random.Random(SEED)
    tracks = []
    def part(name, stage, params, gain, pan, sample=False):
        item = dict(name=name, stage=stage, params=dict(sequencer_on=False, **params),
                    gain=gain, pan=pan, sample=sample, notes=[], controllers=[])
        tracks.append(item)
        return item
    def note(t, p, seconds, length, velocity):
        end = min(stage_seconds, seconds+length)
        if end > seconds:
            t['notes'].append([p, round(seconds,6), round(end,6), velocity])

    dust = part('01_dust',0,dict(source='Random',chop_overtone=110.,chop_comb=3,
                fold_drive=4.,fold_mix=35.,cutoff=4200.,attack=.001,decay=.025,
                sustain=0.,release=.012,stereo_width=.5),.32,-.25)
    clock = part('02_coalescence',0,dict(waveform='Square',chop_overtone=160.,chop_comb=4,
                cutoff=1700.,attack=.001,decay=.045,sustain=.08,release=.03,
                stereo_width=.35),.27,.25)
    anchor = part('03_fingerprint',0,dict(waveform='Sine',attack=.012,decay=.3,
                 sustain=.7,release=.4,cutoff=5000.,stereo_width=0.),.35,0)
    # Density rises while timing scatter decreases: scattered events become a pulse.
    # This remains note-level scheduling, not access to the synth's internal grains.
    t=0.
    while t < stage_seconds:
        x=t/stage_seconds
        note(dust,rng.choice([57,64,69,72,76,81]),t,.025+.12*x,rng.randint(42,100))
        t += rng.uniform(.65,1.35)/(2+24*x*x)
    t=stage_seconds*.25
    while t < stage_seconds:
        x=t/stage_seconds
        jitter=rng.uniform(-.03,.03)*(1-x)
        note(clock,[57,64,69,64][round(t/.125)%4],max(0,t+jitter),.07+.18*x,78)
        t+=.125
    for t in range(0,int(stage_seconds)-4,12):
        for i,p in enumerate([69,72,71,64]):note(anchor,p,t+i, .8,80)

    for stage in [1,2]:
        memory=part(f'{stage+1}1_recognizable_memory',stage,
                    dict(source='Sample',sample_root=69,sample_loop='One-shot',
                         attack=.03,decay=.5,sustain=.8,release=.8,cutoff=4000.,
                         stereo_width=.6,vibrato_depth=15.),.25,-.15,True)
        cloud=part(f'{stage+1}2_dissolved_memory',stage,
                   dict(source='Granular',sample_root=69,grain_size=180.,grain_density=45.,
                        grain_spray=.5,grain_pitch_jitter=12.,grain_scan=.04,
                        attack=1.,decay=1.,sustain=.75,release=4.,cutoff=2600.,
                        stereo_width=1.,lfo_shape='Sine',lfo_sync=False,
                        lfo_rate=.08,lfo_amp=24.,vibrato_depth=10.),.3,.1,True)
        sparks=part(f'{stage+1}3_swarm_sparks',stage,
                    dict(source='Granular',sample_root=69,grain_size=25.,grain_density=70.,
                         grain_spray=.7,grain_pitch_jitter=30.,grain_scan=.1,
                         attack=.003,decay=.12,sustain=.1,release=.15,
                         filter_mode='High-pass',cutoff=3600.,stereo_width=.8),.17,.3,True)
        center=part(f'{stage+1}4_identity',stage,
                    dict(waveform='Triangle',attack=.03,decay=.5,sustain=.65,
                         release=1.2,cutoff=2000.,stereo_width=0.,vibrato_depth=20.),.27,0)
        # Root=69 is a playback reference, not a claim that the mixed sample is in A.
        # Transpositions are intentional musical shifts of a complex recording.
        for t in range(0,int(stage_seconds)-8,16):
            note(memory,[69,64,72,69][(t//16)%4],t,6,65)
        for t in range(4,int(stage_seconds)-8,12):
            for p in [57,64,69]:note(cloud,p,t,10,65)
        t=stage_seconds*.15
        while t<stage_seconds*.85:
            note(sparks,rng.choice([76,81,83,88]),t,.12,rng.randint(38,75))
            t+=rng.uniform(.15,.75)
        # At least one phrase: in --preview (20 s stages) the range would otherwise be empty
        # and the part silent. Full-length stages get exactly the same phrases as before.
        first=int(stage_seconds*.6)
        for t in range(first,max(first+1,int(stage_seconds)-8),12):
            for i,p in enumerate([69,72,71,64]):note(center,p,t+i*2,1.8,75)
        for item in [memory,cloud,center]:
            for t in range(0,int(stage_seconds),2):
                value=round(45*(.5-.5*math.cos(2*math.pi*t/24)))
                item['controllers'].append([1,value,t])
            item['controllers'].append([1,0,stage_seconds])
    for item in tracks:item['notes'].sort(key=lambda n:(n[1],n[0]))
    return tracks


def vlq(n):
    b=[n&127];n>>=7
    while n:b.insert(0,(n&127)|128);n>>=7
    return bytes(b)


def midi_bytes(tracks,stage_seconds):
    ppq=960
    def meta(kind,text):return bytes([255,kind])+vlq(len(text))+text
    def chunk(events):
        events.sort(key=lambda e:(e[0],e[1]));data=bytearray();last=0
        for tick,priority,payload in events:
            data.extend(vlq(tick-last)+payload);last=tick
        end=round((3*stage_seconds+TAIL)/BEAT*ppq)
        data.extend(vlq(max(0,end-last))+b'\xff\x2f\x00')
        return b'MTrk'+struct.pack('>I',len(data))+data
    conductor=[(0,0,meta(3,b'Phase Transitions')),
               (0,0,meta(81,(500000).to_bytes(3,'big'))),
               (0,0,meta(88,bytes([4,2,24,8])))]
    for stage,name in enumerate(['Dust to machinery','Machinery to voice','Memory to atmosphere']):
        conductor.append((round(stage*stage_seconds/BEAT*ppq),0,meta(6,name.encode())))
    chunks=[chunk(conductor)]
    channels=[0,1,2,3,4,5,6,7,8,10,11]
    for item,ch in zip(tracks,channels):
        events=[(0,0,meta(3,item['name'].encode()))]
        offset=item['stage']*stage_seconds
        for p,on,off,v in item['notes']:
            events.extend([(round((offset+on)/BEAT*ppq),2,bytes([0x90|ch,p,v])),
                           (round((offset+off)/BEAT*ppq),1,bytes([0x80|ch,p,0]))])
        for cc,v,t in item['controllers']:
            events.append((round((offset+t)/BEAT*ppq),0,bytes([0xb0|ch,cc,v])))
        chunks.append(chunk(events))
    return b'MThd'+struct.pack('>IHHH',6,1,len(chunks),ppq)+b''.join(chunks)


def curves(item,t,duration):
    x=min(1,max(0,t/duration));name=item['name']
    if name=='01_dust':return dict(cutoff=1200+5000*x,fold_mix=20+55*x)
    if name=='02_coalescence':return dict(cutoff=800+2400*x,chop_overtone=150-90*x)
    if 'dissolved_memory' in name:
        return dict(grain_position=.5+.4*math.sin(2*math.pi*t/53),
                    grain_size=45+350*x,grain_density=20+65*x,
                    cutoff=900+2300*(.5+.5*math.sin(2*math.pi*t/71)))
    if 'swarm_sparks' in name:return dict(grain_position=.5+.45*math.sin(2*math.pi*t/13))
    return {}


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--vst')
    ap.add_argument('--out',type=Path,default=Path('phase_transitions'))
    ap.add_argument('--preview',action='store_true',help='Three 20-second stages plus tail; tests ALL sample generations')
    ap.add_argument('--base-renderer',type=Path,default=Path(__file__).with_name('render_machine_remembers.py'))
    args=ap.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    duration=20. if args.preview else STAGE
    tracks=make_score(duration)
    (args.out/'Phase_Transitions.mid').write_bytes(midi_bytes(tracks,duration))
    (args.out/'score.json').write_text(json.dumps(dict(bpm=BPM,stage_seconds=duration,
        seed=SEED,tracks=tracks,limitations='MIDI does not contain audio assets, native patch settings, '
        'parameter curves or moving panning.'),indent=2),encoding='utf-8')
    if not args.vst:
        print('Score and MIDI exported. Audio not requested.');return
    import numpy as np
    import mido
    import pedalboard
    base=runpy.run_path(str(args.base_renderer),run_name='phase_base')
    for required in ['load_sample','Progress']:
        if required not in base:raise ValueError('Use the latest base renderer with setup and base64 fixes')
    vst=Path(args.vst).expanduser().resolve()
    inner=vst/'Contents'/'x86_64-win'/vst.name
    if sys.platform=='win32' and inner.is_file():vst=inner
    if not vst.exists():raise FileNotFoundError(vst)
    def configure(item,sample=None):
        print('Setup: '+item['name'],flush=True)
        plugin=pedalboard.load_plugin(str(vst))
        params=item['params']
        missing=(set(params)|set(curves(item,0,duration)))-set(plugin.parameters)
        if missing:raise ValueError('Missing plugin parameters: '+str(sorted(missing)))
        if sample:base['load_sample'](plugin,sample)
        for k,v in params.items():setattr(plugin,k,v)
        return plugin
    # Decision: fail incompatible builds before spending time on generations.
    for item in tracks:
        plugin=configure(item);del plugin
    def write24(path,audio):
        with wave.open(str(path),'wb') as w:
            w.setnchannels(2);w.setsampwidth(3);w.setframerate(SR)
            for start in range(0,audio.shape[1],SR*5):
                q=np.ascontiguousarray(np.rint(np.clip(audio[:,start:start+SR*5].T,
                    -1,1-1/8388608)*8388608).astype('<i4'))
                w.writeframesraw(q.view('u1').reshape(-1,4)[:,:3].tobytes())
    def render_part(item,sample):
        plugin=configure(item,sample)
        total=round((duration+TAIL)*SR);chunk=1200
        result=np.zeros((2,total),dtype=np.float32)
        events=[]
        for p,on,off,v in item['notes']:
            events.extend([(round(on*SR),2,mido.Message('note_on',note=p,velocity=v)),
                           (round(off*SR),1,mido.Message('note_off',note=p))])
        for cc,v,t in item['controllers']:
            events.append((round(t*SR),0,mido.Message('control_change',control=cc,value=v)))
        events.sort(key=lambda e:(e[0],e[1]));index=0
        progress=base['Progress'](item['name'],total)
        # Decision: a moving stereo balance preserves existing stereo data; it is not
        # per-grain spatialization. Keeping identity near center makes it recognizable.
        excursion=.07 if 'identity' in item['name'] or 'fingerprint' in item['name'] else .35
        try:
            for start in range(0,total,chunk):
                n=min(chunk,total-start);messages=[]
                while index<len(events) and events[index][0]<start+n:
                    messages.append(events[index][2].copy(time=(events[index][0]-start)/SR));index+=1
                for k,v in curves(item,start/SR,duration).items():setattr(plugin,k,v)
                a=plugin.process(messages,n/SR,SR,buffer_size=256,reset=start==0)
                if a.shape!=(2,n) or not np.isfinite(a).all():raise ValueError('Invalid plugin audio')
                seconds=(start+np.arange(n))/SR
                pan=np.clip(item['pan']+excursion*np.sin(2*np.pi*seconds/37+item['stage']),-.8,.8)
                balance=np.stack((np.sqrt(1-pan),np.sqrt(1+pan)))
                # Final stage simplifies rather than intensifies: fade sparks early,
                # leaving the recurring fingerprint and a thinning granular atmosphere.
                fade=np.minimum(1,np.maximum(0,(duration+TAIL-seconds)/TAIL))**2
                if item['stage']==2 and 'swarm_sparks' in item['name']:
                    fade*=np.clip((duration*.9-seconds)/(duration*.25),0,1)
                result[:,start:start+n]=a*balance*fade*item['gain']
                progress.update(start+n)
        finally:progress.finish()
        del plugin
        if not np.any(np.abs(result)>1e-9):raise ValueError('Silent part: '+item['name'])
        return result
    assets=args.out/'assets';assets.mkdir(exist_ok=True)
    stems=args.out/'stems';stems.mkdir(exist_ok=True)
    total=round((3*duration+TAIL)*SR)
    scratch=args.out/'phase_mix.float64.tmp'
    mix=np.memmap(scratch,dtype='float64',mode='w+',shape=(2,total));mix[:]=0
    reports=[];previous=None
    for stage in range(3):
        stage_mix=np.zeros((2,round((duration+TAIL)*SR)),dtype=np.float64)
        for item in [t for t in tracks if t['stage']==stage]:
            audio=render_part(item,previous if item['sample'] else None)
            peak=float(np.max(np.abs(audio)))
            # Stems have local time zero. The manifest supplies exact placement offsets.
            # A safety reduction only applies if a stem would clip on PCM export.
            stem_scale=min(1.,.891250938/max(peak,1e-12))
            write24(stems/(item['name']+'.wav'),audio*stem_scale)
            reports.append(dict(name=item['name'],offset_seconds=stage*duration,
                                peak=peak,stem_export_scale=stem_scale))
            stage_mix+=audio
            del audio
        start=round(stage*duration*SR)
        mix[:,start:start+stage_mix.shape[1]]+=stage_mix
        if stage<2:
            # Decision: only a fixed 12-second middle passage becomes the next source.
            # This stays below NeonSynth's documented sample-duration cap and avoids
            # runaway recursive feedback. There are exactly two transfers, no loop.
            excerpt_len=min(12.,duration*.6)
            center=duration*.6
            begin=round(max(0,center-excerpt_len/2)*SR)
            excerpt=stage_mix[:,begin:begin+round(excerpt_len*SR)].copy()
            edge=min(round(.03*SR),excerpt.shape[1]//4)
            excerpt[:,:edge]*=np.linspace(0,1,edge)
            excerpt[:,-edge:]*=np.linspace(1,0,edge)
            peak=float(np.max(np.abs(excerpt)))
            if peak<=1e-9:raise ValueError('Silent resampling excerpt')
            excerpt*=.75/peak
            previous=assets/f'generation_{stage+1}.wav'
            write24(previous,excerpt)
        del stage_mix
    # Decision: one master gain preserves relative dynamics. This is sample-peak
    # normalization only, not a true-peak limiter or LUFS mastering chain.
    peak=0.
    for i in range(0,total,SR*5):peak=max(peak,float(np.max(np.abs(mix[:,i:i+SR*5]))))
    if not math.isfinite(peak) or peak<=1e-9:raise ValueError('Invalid master mix')
    master_gain=.891250938/peak
    mix*=master_gain
    write24(args.out/'Phase_Transitions.wav',mix)
    mix.flush();del mix;scratch.unlink()
    (args.out/'render_report.json').write_text(json.dumps(dict(duration_seconds=3*duration+TAIL,
        sample_rate=SR,master_gain=master_gain,stems=reports,plugin=str(vst),
        auditioned=False,stem_note='Stems are local-time; place at offset_seconds. '
        'Undo any stem_export_scale, sum, then apply master_gain to match the master.'),indent=2))
    print('Wrote master, stems, resampling assets, MIDI and placement report. Listen before calling it final.')

if __name__=='__main__':main()
