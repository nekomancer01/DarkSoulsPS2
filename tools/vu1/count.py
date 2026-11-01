"""Cycle count of a .vsm loop: python3 count.py file.vsm entry_label loop_label [iterations]
Prints prologue cycles, cycles per loop iteration (steady state) and stall cycles per iteration."""
import sys
from vucore import parse_program, count

def loop_cycles(path, entry, loop, iters=6):
    pairs, labels = parse_program(open(path).read())
    start = labels[entry]
    loop_idx = labels[loop]
    cyc, trace, stalls = count(pairs, start, take_branch=lambda i, n: n < iters)
    visits = [c for i, c in trace if i == loop_idx]
    per = (visits[-1] - visits[1]) / (len(visits) - 2)
    return visits[0], per, cyc - visits[-1], stalls

if __name__ == '__main__':
    p, e, l = sys.argv[1:4]
    pro, per, epi, st = loop_cycles(p, e, l, int(sys.argv[4]) if len(sys.argv) > 4 else 6)
    print('prologue %d, per iteration %.1f, last iteration + epilogue %d, total stalls %d' % (pro, per, epi, st))
