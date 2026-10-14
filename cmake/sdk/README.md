# RFModel C++17 engineering SDK candidate

This is an internal engineering candidate, not a publicly released SDK.
Distribution-license selection is pending. No new license or grant is supplied
by this archive. Engineering validation does not establish full SystemVue 2023
compatibility. Vendor captures and reference archives are intentionally absent.

The payload is header-only: include/rfmodel contains canonical module headers and
flat compatibility wrappers; lib/cmake/RFModel contains relocatable CMake exports.
No compiled C ABI, CLI, Python runtime, reference tooling, or build cache is included.
The C API declaration header may be present but no C ABI binary is provided.

Run `cmake -DSDK_ROOT=/absolute/path/to/sdk -P verify-sdk.cmake` to check the exact
payload inventory and SHA-256 hashes. The manifest excludes itself; the separate
ZIP SHA-256 covers the entire archive, including the manifest. Hashes detect
accidental alteration; they are not signatures or proof of publisher authenticity.
Use a trusted publisher and distribution channel. The producer-side extractor
bounds compressed input (32 MiB), each payload file (8 MiB), total payload
(64 MiB), member count (512), and name/listing sizes before extraction. This is
an engineering SDK integrity/containment check, not a generic hostile-archive sandbox.

For use, see examples/sdk-consumer/README.md. Configure consumers with
`find_package(RFModel 0.1.0 EXACT CONFIG REQUIRED)` and RFModel::rfmodel.
This pure header package is architecture-independent. Full packages with the
compiled C ABI retain architecture checks.

A clean manifest records the actual Git HEAD whose complete worktree was clean
at packaging time. Developer artifacts explicitly record dirty worktree status
and an observed base HEAD, with no exact source-commit claim. Payload hashes,
not a developer artifact's base HEAD, identify its installed contents.
