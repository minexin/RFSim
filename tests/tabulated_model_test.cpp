#include "rfmodel/tabulated_model.hpp"
#include "rfmodel/linear_analysis.hpp"
#include "test_support.hpp"

int main(int argc, char **argv) {
    using namespace rfmodel;
    require(argc == 2, "fixture path");
    auto model =
        TabulatedSParameterModel::from_touchstone("filter", std::string(argv[1]) + "/sweep.s2p");
    RFDeviceModel &device = model;
    SParameterProvider &provider = model;
    require(device.name() == "filter" && device.port_count() == 2, "device identity");
    near(device.port(1).reference_impedance, 50.);
    near(provider.s_parameters(2e9)(1, 0), 0.375);
    auto analysis = analyze_linear(FrequencyGrid{{1e9, 2e9, 3e9}}, {0, 1}, [&](double f) {
        LinearNetwork network(device.port(0).reference_impedance.real());
        network.add(provider.s_parameters(f), device.port(0).reference_impedance.real());
        return network;
    });
    near(analysis.scattering[1](1, 0), 0.375);
    rejects<std::out_of_range>([&] {
        device.port(2);
    });
    rejects<std::out_of_range>([&] {
        provider.s_parameters(4e9);
    });
    auto data = read_touchstone(std::string(argv[1]) + "/nonreciprocal.s2p");
    TabulatedSParameterModel snapshot("snapshot", data, OutOfBand::Clamp);
    data.matrices[0](1, 0) = 999;
    near(snapshot.s_parameters(9e9)(1, 0), 2.);
    near(snapshot.port(0).reference_impedance, 75.);
    require(snapshot.minimum_frequency_hz() == 1e9, "model domain");
    rejects<std::invalid_argument>([&] {
        TabulatedSParameterModel invalid("", data);
    });
}
