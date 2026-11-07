# nvEncodeAPI.h — where it comes from

| | |
|---|---|
| File | `nvEncodeAPI.h` (NVENC API 13.0), unmodified |
| Source | https://github.com/FFmpeg/nv-codec-headers/blob/n13.0.19.0/include/ffnvcodec/nvEncodeAPI.h |
| Tag | `n13.0.19.0` |
| SHA-256 | `4fe4094541ef0f8a13249d97a8692dc5f835a6e9dd42eeadb3e2f7321d54dc7e` |
| Size | 311 883 bytes |
| Fetched | 2026-11-07, this file only |
| Licence | MIT. NVIDIA's own grant, in the header of the file itself (copied to `LICENSE-nvEncodeAPI.txt`). The repository carries no separate LICENSE file at that tag. |

It is compiled in only to know the structures of the NVENC API version `groovy_nvenc.c` is written against.
The library itself (`nvEncodeAPI64.dll`) is the NVIDIA driver's and is loaded at run time: nothing is linked or shipped.
