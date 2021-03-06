#!/usr/bin/env python

__doc__="""

%prog [options] inputfiles

Concatenate hdf5 LHE files as output by Sherpa

TODO: probably add the npLO and npNLO datasets here
so that there is only one postprocessing step.

"""


def nEvent(fname):
    import h5py
    f = h5py.File(fname)
    ret = f['event/weight'].shape[0]
    f.close()
    return ret

def nPart(fname):
    import h5py
    f = h5py.File(fname)
    ret = f['particle/id'].shape[0]
    f.close()
    return ret


import numpy as np
import h5py

if __name__ == "__main__":
    import optparse, os, sys
    op = optparse.OptionParser(usage=__doc__)
    op.add_option("-v", "--debug", dest="DEBUG", action="store_true", default=False, help="Turn on some debug messages")
    op.add_option("-c", dest="COMPRESSION", type=int, default=4, help="GZip compression level (default: %default)")
    op.add_option("-o", dest="OUTFILE", default="merged.h5", help="Output file name (default: %default)")
    op.add_option("-f", "--FORCE", dest="FORCE", action="store_true", default=False, help="Overwrite output file if it exists (default: %default)")
    opts, args = op.parse_args()


    # Collect dataset sizes from files
    NP = [nPart(f)  for f in args]
    NE = [nEvent(f) for f in args]
    NPtot = sum(NP)
    NEtot = sum(NE)

    # Open existing new H5 for writing
    f = h5py.File(opts.OUTFILE, "w")

    # This copies the structure
    # NOTE the init and procinfo things realy are attributes and
    # don't change from file to file so we just copy them from the
    # first file to the output file.
    g = h5py.File(args[0])
    struct = []
    g.visit(struct.append)
    ds_I = [x for x in struct if x.startswith("index")    and "/" in x]
    ds_e = [x for x in struct if x.startswith("event")    and "/" in x]
    ds_p = [x for x in struct if x.startswith("particle") and "/" in x]
    ds_i = [x for x in struct if x.startswith("init")     and "/" in x]
    ds_P = [x for x in struct if x.startswith("procInfo") and "/" in x]
    # Empty dataset but with correct size and dtype
    for _I in ds_I: f.create_dataset(_I, (NEtot,),   dtype=g[_I].dtype, compression=opts.COMPRESSION)
    for _e in ds_e: f.create_dataset(_e, (NEtot,),   dtype=g[_e].dtype, compression=opts.COMPRESSION)
    for _p in ds_p: f.create_dataset(_p, (NPtot,),   dtype=g[_p].dtype, compression=opts.COMPRESSION)
    # Full dataset TODO think about makeing those attributes???
    for _i in ds_i: f.create_dataset(_i, data=g[_i], dtype=g[_i].dtype, compression=opts.COMPRESSION)
    for _P in ds_P: f.create_dataset(_P, data=g[_P], dtype=g[_P].dtype, compression=opts.COMPRESSION)
    g.close()

    # Iterate over ALL input files
    for num, fname in enumerate(args):
        if num ==0:
            e_start = 0
            p_start = 0
        else:
            e_start=NE[num]
            p_start=NP[num]
        e_end = e_start + NE[num]
        p_end = p_start + NP[num]
        if opts.DEBUG:
            print ("events from {} to {}".format(e_start, e_end))
            print ("particles from {} to {}".format(p_start, p_end))
        g = h5py.File(fname)
        for _e in ds_e: f[_e][e_start:e_end] = g[_e]
        for _p in ds_p: f[_p][p_start:p_end] = g[_p]
        for _I in ds_I: f[_I][e_start:e_end] = g[_I][:]+ p_start
        g.close()

    f.close()
    sys.exit(0)
