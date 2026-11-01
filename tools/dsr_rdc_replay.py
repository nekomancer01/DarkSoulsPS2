# Runs inside RenderDoc's qrenderdoc (`qrenderdoc --python dsr_rdc_replay.py`, its embedded Python 3.8 + renderdoc
# module); started by tools/dsr-rdc.sh. Replays a DSR capture (DXVK -> Vulkan) and writes, for every draw / dispatch,
# the SPIR-V sha1 and DXVK dump name of each bound shader and the bytes of every bound constant buffer. dsr_rdc.py names them afterwards
# (SPIR-V -> DXVK's dumped DXBC -> RDEF variable names -> the shaderbnd entry).
# In: env RDC_IN (capture), RDC_OUT (JSON). Out: {"events": [...], "blobs": {sha1: hex}, "textures": [...]}.
import hashlib, json, os, re, traceback

import renderdoc as rd

STAGES = [('vs', rd.ShaderStage.Vertex), ('ps', rd.ShaderStage.Pixel), ('cs', rd.ShaderStage.Compute)]


def main():
    out = os.environ['RDC_OUT']
    log = open(out + '.log', 'w')
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(os.environ['RDC_IN'], '', None) != rd.ResultCode.Succeeded:
        raise RuntimeError('cannot open capture')
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    if res != rd.ResultCode.Succeeded:
        raise RuntimeError('cannot replay: %s' % res)
    sd = ctl.GetStructuredFile()
    events, blobs, spv = [], {}, {}

    def walk(actions):
        for a in actions:
            if a.children:
                walk(a.children)
            if not (a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch)):
                continue
            ctl.SetFrameEvent(a.eventId, False)
            pipe = ctl.GetPipelineState()
            ev = {'eid': a.eventId, 'name': a.GetName(sd), 'stages': {}}
            for sname, st in STAGES:
                refl = pipe.GetShaderReflection(st)
                if refl is None:
                    continue
                raw = bytes(refl.rawBytes)
                h = hashlib.sha1(raw).hexdigest()
                # DXVK names each module after its dump file (OpString "fs.<hash>", DXVK_SHADER_DUMP_PATH)
                m = re.search(rb'(?:vs|fs|cs|gs|hs|ds)\.[0-9a-f]{32}', raw)
                spv[h] = m.group().decode() if m else None
                cbs = []
                for i, cb in enumerate(refl.constantBlocks):
                    if not cb.bufferBacked:
                        continue
                    try:
                        d = pipe.GetConstantBlock(st, i, 0).descriptor
                        size = d.byteSize if d.byteSize and d.byteSize < (1 << 20) else cb.byteSize
                        data = bytes(ctl.GetBufferData(d.resource, d.byteOffset, size))
                    except Exception as e:
                        log.write('eid %d %s cb %d: %s\n' % (a.eventId, sname, i, e))
                        continue
                    bh = hashlib.sha1(data).hexdigest()
                    blobs.setdefault(bh, data.hex())
                    cbs.append({'index': i, 'name': cb.name, 'set': cb.fixedBindSetOrSpace,
                                'binding': cb.fixedBindNumber, 'size': len(data), 'blob': bh})
                ev['stages'][sname] = {'spv_sha1': h, 'dxvk': spv[h], 'cbs': cbs}
            events.append(ev)
            if len(events) % 200 == 0:
                log.write('%d events\n' % len(events))
                log.flush()

    walk(ctl.GetRootActions())
    json.dump({'events': events, 'blobs': blobs}, open(out, 'w'))
    log.write('done: %d events, %d blobs\n' % (len(events), len(blobs)))
    log.close()
    ctl.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    open(os.environ['RDC_OUT'] + '.err', 'w').write(traceback.format_exc())
os._exit(0)
