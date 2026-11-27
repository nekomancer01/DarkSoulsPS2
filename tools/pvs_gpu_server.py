"""The GPU side of the container ISO build (tools/build-iso.sh): runs the occlusion bakes the build container hands over.

  pvs_gpu_server.py <spool dir>

tools/build-iso.sh starts this in a small glibc container (tools/iso-builder/Dockerfile.gpu) that has the GPU, with the
same /src and /build mounts as the ps2dev build container, and stops it when the build is done. It builds pvs_bake
with the GPU path (tools/pvs_gpu.c) from the checkout, checks that it finds a GPU (pvs_bake --gpu-probe), then keeps
<spool>/ready fresh and runs every <name>.job the build's tools/pvs_cache.py writes: a JSON {"argv", "cwd"} whose
argv[0] (the build container's own pvs_bake) is replaced by this one. It writes <name>.log (the bake's output) and then
<name>.done (its exit code). Without a GPU it writes <spool>/nogpu and exits: the build bakes on its own CPU."""
import concurrent.futures, json, os, platform, subprocess, sys, time

# pvs_bake's OpenMP threads sleep between its many short parallel loops instead of spinning (libgomp's default spins):
# with the GPU path a bake spun ~200 of its ~270 CPU-s per Burg block, starving the rest of the build (2026-11-17);
# the bytes are the same either way
os.environ.setdefault("OMP_WAIT_POLICY", "passive")

HERE = os.path.dirname(os.path.abspath(__file__))
JOBS = int(os.environ.get("PVS_GPU_JOBS", "2"))  # bakes at once (each one's draw lists use every core)


def build(out):
    # the same sources as $(PVS_BAKE) in ps2/proto/export_rules.mk: keep the two lists in step
    proto = os.path.join(HERE, "..", "ps2", "proto", "src")
    src = [os.path.join(HERE, "pvs_bake.c"), os.path.join(HERE, "pvs_gpu.c"), os.path.join(proto, "chunk.c")]
    deps = [os.path.join(HERE, "pvs_gpu.h"), os.path.join(HERE, "native", "chunkw.h"), os.path.join(proto, "chunk.h"),
            os.path.join(proto, "vu1_layout.h")]
    if os.path.exists(out) and os.path.getmtime(out) >= max(os.path.getmtime(s) for s in src + deps):
        return True
    tmp = f"{out}.{os.getpid()}"
    r = subprocess.run(["gcc", "-O3", "-march=native", "-ffp-contract=off", "-fopenmp", "-DPVS_GPU", "-o", tmp] + src +
                       ["-lEGL", "-lOpenGL", "-lm"], capture_output=True, text=True)
    if r.returncode:
        print("pvs_gpu_server: build failed:\n" + r.stderr[-2000:], flush=True)
        return False
    os.replace(tmp, out)
    return True


def run(spool, bake, name):
    with open(os.path.join(spool, name + ".run")) as f:
        job = json.load(f)
    r = subprocess.run([bake] + job["argv"][1:], cwd=job.get("cwd") or None, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    with open(os.path.join(spool, name + ".log.tmp"), "w") as f:
        f.write(r.stdout)
    os.replace(os.path.join(spool, name + ".log.tmp"), os.path.join(spool, name + ".log"))
    with open(os.path.join(spool, name + ".done.tmp"), "w") as f:
        f.write(str(r.returncode))
    os.replace(os.path.join(spool, name + ".done.tmp"), os.path.join(spool, name + ".done"))
    os.remove(os.path.join(spool, name + ".run"))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    spool = sys.argv[1]
    os.makedirs(spool, exist_ok=True)
    for f in os.listdir(spool):  # a stopped run's leftovers
        if f.endswith((".job", ".run", ".done", ".log", ".tmp")) or f in ("ready", "nogpu", "stop"):
            os.remove(os.path.join(spool, f))
    bake = os.path.join(spool, "pvs_bake-gpu-" + "".join(platform.libc_ver()))  # (per C library: one built elsewhere may not run here)
    ok = build(bake)
    probe = subprocess.run([bake, "--gpu-probe"], capture_output=True, text=True) if ok else None
    if not ok or probe.returncode:
        open(os.path.join(spool, "nogpu"), "w").close()
        print("pvs_gpu_server: no GPU here: the build bakes on the CPU", flush=True)
        return
    ren = probe.stdout.strip()
    print(f"pvs_gpu_server: baking on {ren}", flush=True)
    busy = set()
    with concurrent.futures.ThreadPoolExecutor(JOBS) as pool:
        while not os.path.exists(os.path.join(spool, "stop")):
            with open(os.path.join(spool, "ready.tmp"), "w") as f:
                f.write(ren + "\n")
            os.replace(os.path.join(spool, "ready.tmp"), os.path.join(spool, "ready"))
            for f in sorted(os.listdir(spool)):
                if f.endswith(".job") and len(busy) < JOBS:
                    name = f[:-4]
                    try:
                        os.rename(os.path.join(spool, f), os.path.join(spool, name + ".run"))  # claimed
                    except OSError:
                        continue
                    fut = pool.submit(run, spool, bake, name)
                    busy.add(fut)
                    fut.add_done_callback(busy.discard)
            time.sleep(0.2)


if __name__ == "__main__":
    main()
