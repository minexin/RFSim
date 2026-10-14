#include "rfmodel/adapters/builtin_model_adapters.hpp"
#include "rfmodel/models/ideal_devices.hpp"
#include "../support/test_support.hpp"
#include <iostream>

using namespace rfmodel;

namespace {
constexpr double kt = 1.380649e-23 * 290.;

void mixer_noise() {
    // Owned snapshot remains usable after its adapter and providers are destroyed.
    auto pad = [] {
        return adapt_passive_model(MatchedTransmissionModel("pad", 20. * std::log10(2.)),
                                   {"test.pad", "1", "", ""},
                                   290.)
            .conversion_device(1e8, {8, 12}, ModelNoisePolicy::require_model_noise);
    }();
    require(pad.model.channels()[2].port == 0 && pad.model.channels()[2].bin == 12,
            "channel ordering changed");
    pad.source[2] = {1., .2};
    pad.source_noise.covariance(0, 0) = kt;
    pad.source_noise.covariance(2, 2) = kt;
    FrequencyConversionNetwork network(1e8);
    network.add(pad);
    network.add(ideal_mixer_conversion(1e8, {{0, 8}, {0, 12}, {1, 2}, {1, 18}, {1, 22}}, 10, 0.));
    network.connect(0, 1, 1, 0);
    const auto result = network.analyze();
    near(result.outgoing[6], {.5, .1});
    // Each independent RF sideband contains kT after a pad at equilibrium.
    near(result.outgoing_noise.covariance(6, 6) / kt, 2.);
    near(result.outgoing_noise.complementary(6, 6) / kt, 0.);
    // The lower RF sideband contributes conjugated noise to IF, and ordinary
    // noise to the sum sideband. Their complementary correlation is kT.
    near(result.outgoing_noise.complementary(6, 7) / kt, 1.);
    near(result.outgoing_noise.covariance(6, 8) / kt, 1.);
    near(network.reference_noise_analysis({2}, {0, 2}, 6).noise_factor, 8.);
    near(network.reference_noise_analysis({0, 2}, {0, 2}, 6).noise_factor, 4.);
}

void frequency_samples_and_dc() {
    std::vector<double> sampled, noise_sampled;
    ModelFunctions hooks;
    hooks.scattering = [&](double hz) {
        sampled.push_back(hz);
        return SMatrix{1, {hz * .01}};
    };
    hooks.noise = [&](double hz) {
        noise_sampled.push_back(hz);
        return NoiseCorrelation{{1, {(1. + hz) * kt}}};
    };
    ModelAdapter model({{"test.table", "1", "", ""}, "table", {{0, "p", 75.}}}, hooks);
    const auto device =
        model.conversion_device(2., {0, 2, 5}, ModelNoisePolicy::require_model_noise);
    require(sampled == std::vector<double>({0., 4., 10.}) && noise_sampled == sampled,
            "providers must sample each frequency exactly once");
    near(device.model.direct()(2, 2), .1);
    near(device.model.direct()(1, 2), 0.);
    near(device.intrinsic_noise.covariance(1, 1) / kt, 5.);
    near(device.intrinsic_noise.covariance(2, 2) / kt, 11.);
    near(device.intrinsic_noise.complementary(0, 0) / kt, 1.);
    near(device.intrinsic_noise.complementary(1, 1) / kt, 0.);
    FrequencyConversionNetwork network(2.);
    network.add_power_waves(device);
    const auto result = network.analyze();
    near(result.outgoing_noise.covariance(0, 0) / kt, 1.);
    near(result.outgoing_noise.complementary(0, 0) / kt, 1.);
    noise_sampled.clear();
    const auto quiet = model.conversion_device(2., {0, 2}, ModelNoisePolicy::omit_intrinsic_noise);
    require(noise_sampled.empty(), "omission invoked noise provider");
    near(quiet.intrinsic_noise.covariance(0, 0), 0.);
    hooks.noise = {};
    ModelAdapter without_noise(model.descriptor(), hooks);
    rejects<ModelCapabilityUnavailable>([&] {
        without_noise.conversion_device(2., {1}, ModelNoisePolicy::require_model_noise);
    });
    without_noise.conversion_device(2., {1}, ModelNoisePolicy::omit_intrinsic_noise);
}

void complex_reference_feedback() {
    const Complex z{40., 15.}, gamma{.2, -.1}, source{.8, .3};
    constexpr double resistance = 90.;
    const Complex s = (resistance - std::conj(z)) / (resistance + z);
    const double covariance = kt * 4. * resistance * z.real() / std::norm(resistance + z);
    ModelFunctions hooks;
    hooks.scattering = [s](double) {
        return SMatrix{1, {s}};
    };
    hooks.noise = [covariance](double) {
        return NoiseCorrelation{{1, {covariance}}};
    };
    ModelAdapter model({{"test.resistor", "1", "", ""}, "resistor", {{0, "p", z}}}, hooks);
    auto device = model.conversion_device(1e8, {1, 3}, ModelNoisePolicy::require_model_noise);
    require(device.model.reference_impedances_ohms() == std::vector<Complex>({z, z}),
            "complex reference lost during lift");
    device.source = {source, 2. * source};
    device.reflection = {gamma, gamma};
    FrequencyConversionNetwork network(1e8);
    network.add_power_waves(device);
    const auto actual = network.analyze();
    for (std::size_t i = 0; i < 2; ++i) {
        near(actual.outgoing[i], (i + 1.) * s * source / (1. - s * gamma));
        near(actual.outgoing_noise.covariance(i, i) / kt,
             covariance / kt / std::norm(1. - s * gamma));
    }
    near(actual.outgoing_noise.covariance(0, 1) / kt, 0.);
}

void invalid_contracts() {
    int calls = 0;
    ModelFunctions hooks;
    hooks.scattering = [&](double) {
        ++calls;
        return SMatrix{1, {0.}};
    };
    ModelDescriptor descriptor{{"test.invalid", "1", "", ""}, "test", {{0, "p", 50.}}};
    ModelAdapter model(descriptor, hooks);
    const auto policy = ModelNoisePolicy::omit_intrinsic_noise;
    for (const auto &bins : std::vector<std::vector<int>>{{}, {-1}, {2, 1}, {1, 1}, {1, -1}}) {
        rejects<std::invalid_argument>([&] {
            model.conversion_device(1., bins, policy);
        });
    }
    for (double spacing : {0., -1., std::numeric_limits<double>::infinity()}) {
        rejects<std::invalid_argument>([&] {
            model.conversion_device(spacing, {1}, policy);
        });
    }
    rejects<std::invalid_argument>([&] {
        model.conversion_device(std::numeric_limits<double>::max(), {1, 2}, policy);
    });
    rejects<std::invalid_argument>([&] {
        model.conversion_device(1., std::vector<int>(513, 1), policy);
    });
    rejects<std::invalid_argument>([&] {
        model.conversion_device(1., {1}, static_cast<ModelNoisePolicy>(42));
    });
    descriptor.ports[0].reference_impedance = {50., 1.};
    ModelAdapter complex(descriptor, hooks);
    rejects<std::invalid_argument>([&] {
        complex.conversion_device(1., {0, 1}, policy);
    });
    require(calls == 0, "invalid request reached provider");
    descriptor.ports[0].reference_impedance = 50.;
    hooks.scattering = [](double) {
        return SMatrix{1, {Complex{0., .2}}};
    };
    ModelAdapter imaginary_dc(descriptor, hooks);
    rejects<std::invalid_argument>([&] {
        imaginary_dc.conversion_device(1., {0}, policy);
    });
    hooks.scattering = [](double) {
        return SMatrix{1, {0.}};
    };
    hooks.noise = [](double) {
        return NoiseCorrelation{{1, {-kt}}};
    };
    ModelAdapter negative_noise(descriptor, hooks);
    rejects<std::invalid_argument>([&] {
        negative_noise.conversion_device(1., {1}, ModelNoisePolicy::require_model_noise);
    });
    descriptor.ports.push_back({1, "q", 50.});
    hooks.scattering = [](double) {
        return SMatrix{2, {0., 0., 0., 0.}};
    };
    hooks.noise = [](double) {
        return NoiseCorrelation{{2, {kt, Complex{0., .5 * kt}, Complex{0., -.5 * kt}, kt}}};
    };
    ModelAdapter imaginary_dc_noise(descriptor, hooks);
    rejects<std::invalid_argument>([&] {
        imaginary_dc_noise.conversion_device(1., {0}, ModelNoisePolicy::require_model_noise);
    });
    // This Hermitian PSD covariance is valid for stationary positive-frequency noise.
    imaginary_dc_noise.conversion_device(1., {1}, ModelNoisePolicy::require_model_noise);
}
} // namespace

int main() {
    try {
        mixer_noise();
        frequency_samples_and_dc();
        complex_reference_feedback();
        invalid_contracts();
        std::cout << "model conversion adapters passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
