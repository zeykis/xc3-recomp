# hactool (SciresM, ISC licence)

`hactool.exe` reads your own game dump in XC3 Setup (NCA / NSP / XCI decryption and extraction).

Built from https://github.com/SciresM/hactool at commit `1d64a83450e025622f3468c28fc4164dad2c5ef6` (master,
newer than the 1.4.0 release): 1.4.0 misreads small blocks of encrypted sections ("Invalid BKTR layout!" on the
2.2.1 update), fixed after it. With one patch, `bktr-layout.patch`, for the newer patch layout of updates such as 2.2.1: more tables follow the
BKTR subsection table (hactool stopped with "Invalid BKTR layout!"), and the AES-CTR-Ex table covers the data only,
up to the relocation table (hactool called the section corrupted and extracted nothing). Build (mingw-w64 GCC 13, static, from that commit with the patch applied:
`git apply bktr-layout.patch`):

```
printf 'CC = x86_64-w64-mingw32-gcc\nAR = x86_64-w64-mingw32-ar\nCFLAGS = -O2 -Wall -std=gnu11\nLDFLAGS = -lmbedtls -lmbedx509 -lmbedcrypto -static\nEXEEXT = .exe\n' > config.mk
make CC=x86_64-w64-mingw32-gcc AR=x86_64-w64-mingw32-ar
```

`build-windows.bat` copies it to `tools\hactool.exe` and next to `xc3-setup.exe`.
