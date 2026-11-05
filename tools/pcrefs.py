# Usage: python3 -I tools/pcrefs.py game_app/POP decompiled/macho/symbols.txt <hexaddr>...
# Exact PIC xrefs for ARM-mode Mach-O: ldr rX,[pc,#imm] -> (add rY,pc,rX | ldr rY,[pc,rX]) within 16 insns.
import struct,bisect,sys,pickle,os
BIN=sys.argv[1]; SYMS=sys.argv[2]; targets=[int(x,16) for x in sys.argv[3:]]
d=open(BIN,'rb').read()
r=lambda a:struct.unpack_from('<I',d,a-0x1000)[0]
syms=[]
for l in open(SYMS):
    p=l.split()
    if len(p)>=3:
        try: syms.append((int(p[0],16),p[2]))
        except: pass
syms.sort(); ks=[a for a,_ in syms]
fn=lambda a:syms[bisect.bisect_right(ks,a)-1][1]
TS,TE=0x2678,0x662fd4
cache=os.path.join(os.path.dirname(os.path.abspath(SYMS)), "pcrefs.pkl")  # next to symbols.txt (gitignored)
if os.path.exists(cache): refs=pickle.load(open(cache,'rb'))
else:
    refs={}
    for A in range(TS,TE,4):
        i=r(A)
        if (i&0x0f7f0000)==0x051f0000:  # ldr rX,[pc,#+-imm]
            rx=(i>>12)&15; imm=i&0xfff; lit=A+8+(imm if i&0x800000 else -imm)
            if not (TS<=lit<TE): continue
            w=r(lit)
            for B in range(A+4,min(A+64,TE),4):
                j=r(B)
                if (j&0x0ff00ff0)==0x00800000 and ((j>>16)&15)==15 and (j&15)==rx: # add rY,pc,rX
                    refs.setdefault((w+B+8)&0xffffffff,[]).append((A,B,'add')); break
                if (j&0x0ff00ff0)==0x07900000 and ((j>>16)&15)==15 and (j&15)==rx: # ldr rY,[pc,rX]
                    refs.setdefault((w+B+8)&0xffffffff,[]).append((A,B,'ldr')); break
                if (j&0x0ff00ff0)==0x07d00000 and ((j>>16)&15)==15 and (j&15)==rx: # ldrb rY,[pc,rX]
                    refs.setdefault((w+B+8)&0xffffffff,[]).append((A,B,'ldrb')); break
                if ((j>>12)&15)==rx and (j&0x0c000000)==0: break  # rX redefined (data-proc)
    pickle.dump(refs,open(cache,'wb'))
for t in targets:
    print(hex(t))
    for A,B,k in refs.get(t,[]): print('  ',k,hex(B),fn(B))
