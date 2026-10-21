# Small H.264/HEVC with a GPU frame cache instead of DXV

Clips play from ordinary long-GOP H.264/HEVC/ProRes (plus HAP) through the hardware decoder into a bounded frame cache. Reverse, scrubbing and BPM Random Jump are served from that cache, and the next jump target is prefetched.

DXV was only ever used because Resolume decodes just one frame ahead, which makes long-GOP reverse/jump stutter. It is not supported: it is a closed format, and the library holds only 3 DXV files. The prototype showed 0 missed frames on a 14-minute 1080p60 file with 5 s keyframe spacing, in forward, reverse and Random Jump.
