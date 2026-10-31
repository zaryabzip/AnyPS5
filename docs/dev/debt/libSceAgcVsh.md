# libSceAgcVsh technical debt

### Unknown function info

- [The 80 exports that return a libSceAgc function's result](../../../core/libs/prx/libSceAgcVsh/Export.cpp) (libSceAgcVsh) - forward to the libSceAgc function with the same NID. @BunnyFl's [static comparison of firmware 13.42's two modules](https://github.com/boykopovar/AnyPS5/issues/13815#issuecomment-6543804005) found 77 of these bodies byte-identical and 4 the same instructions at different addresses (`sceAgcAcbAcquireMemGetSize`, `sceAgcCreateInterpolantMapping`, `sceAgcLinkShaders` and `sceAgcUpdatePrimState`). Nothing was run, and the data that rip-relative operands point at wasn't compared. On the console VSH imports only libc and libkernel and carries its own copy. Here it links libSceAgc, so `sceAgcLinkShaders` and `sceAgcCreateInterpolantMapping` also run libSceAgc's shader ABI resolution in libSceAgcDriver, as they do for a title.

- The other 70 declarations in [Export.cpp](../../../core/libs/prx/libSceAgcVsh/Export.cpp) are present in the PS5 3.20 [export inventory](https://github.com/DNNDHH/PS5-3.20_Libs/blob/b2a77c09cd71c01f34c790cfbe98c751a13262ad/libSceAgcVsh.c) and the local import-stub table. Their signatures and behavior are unverified; the parameterless declarations are for import resolution only and throw through NotImplemented_nid_no_patch.
