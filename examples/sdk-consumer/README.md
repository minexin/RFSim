# Installed SDK example

This standalone two-translation-unit C++17 example needs only CMake, a C++ compiler,
and the extracted RFModel SDK. It has no paths into the source repository.

    cmake -S examples/sdk-consumer -B example-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/absolute/path/to/extracted-sdk
    cmake --build example-build --config Release
    ctest --test-dir example-build -C Release --output-on-failure

Use quotes around paths containing spaces. `network.cpp` constructs a 6 dB matched
attenuator and a lossless 50 ohm, 1 ns line at each frequency, connects them with
`LinearNetwork`, and solves with a matched 0 dBm source and load. Its returned
`NetworkWaves` owns its vectors after the local models and network are destroyed.
`main.cpp` measures delivered load power and gain and checks the analytic complex
output at 100, 250 and 500 MHz. These checks also run in Release builds.

Frequencies are Hz, powers are W (dBm only for conversion/display), and RMS
power-wave amplitudes are sqrt(W), not volts. Global ports 0/1 belong to the
attenuator and 2/3 to the line. At network output port 3, outgoing minus incident
power is delivered to the external load; it is the negative of power absorbed
into the network. Expected load power is about 0.251189 mW, or -6 dBm.

The host catches an expected negative-frequency exception, then runs the valid
sweep; unexpected exceptions reach the application's nonzero-exit boundary.
This is a small function-API integration example, not a SystemVue compatibility
claim or a frequency-dependent physical cable model.
