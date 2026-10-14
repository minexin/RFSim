# Installed SDK example

This standalone two-translation-unit C++17 example needs only CMake, a C++ compiler,
and the extracted RFModel SDK. It has no paths into the source repository.

    cmake -S examples/sdk-consumer -B example-build -DCMAKE_PREFIX_PATH=/absolute/path/to/extracted-sdk
    cmake --build example-build --config Release
    ctest --test-dir example-build -C Release --output-on-failure

Use quotes around paths containing spaces. The example tests a matched two-port
S matrix; it is an integration smoke test, not a SystemVue compatibility claim.
