# file: n1pt_dart.c
s/for (u64 dva = iova, p = pa; dva < iova + size;/for (u64 dva = iova, p = pa; dva < iova + size - SZ_16K;/
