#include "rfmodel/api/c_api.h"
#include "rfmodel/conversion/conversion_network.hpp"
#include "rfmodel/conversion/conversion_power_wave.hpp"
#include "../support/test_support.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <limits>

using namespace rfmodel;
using Wave = std::vector<Complex>;
using CWave = std::vector<rfmodel_complex>;
static constexpr rfmodel_complex sentinel{719., -317.};

static Complex value(rfmodel_complex z) {
    return {z.real, z.imag};
}

static CWave encode(const Wave &v) {
    CWave result;
    for (auto z : v) {
        result.push_back({z.real(), z.imag()});
    }
    return result;
}

static void ok(int code) {
    require(code == RFMODEL_OK, rfmodel_last_error());
}

static void equal(const CWave &actual, const Wave &expected) {
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto error = std::abs(value(actual[i]) - expected[i]);
        require(error <= 2e-12 * std::max(1e-24, std::abs(expected[i])), "C ABI value differs");
    }
}

static bool identical(const CWave &a, const CWave &b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0;
}

struct EncodedDevice {
    std::vector<size_t> ports;
    std::vector<int> bins;
    CWave a, b, source, reflection, sc, sp, ic, ip, z;
    rfmodel_conversion_request request{};
    rfmodel_conversion_references references{};

    explicit EncodedDevice(const ConversionDevice &device) {
        for (const auto &c : device.model.channels()) {
            ports.push_back(c.port);
            bins.push_back(c.bin);
        }
        a = encode(device.model.direct().values);
        b = encode(device.model.conjugate().values);
        source = encode(device.source);
        reflection = encode(device.reflection);
        sc = encode(device.source_noise.covariance.values);
        sp = encode(device.source_noise.complementary.values);
        ic = encode(device.intrinsic_noise.covariance.values);
        ip = encode(device.intrinsic_noise.complementary.values);
        z = encode(device.model.reference_impedances_ohms());
        request = {ports.size(),
                   device.model.spacing_hz(),
                   std::numeric_limits<double>::quiet_NaN(),
                   ports.data(),
                   bins.data(),
                   a.data(),
                   b.data(),
                   source.data(),
                   reflection.data(),
                   sc.data(),
                   sp.data(),
                   ic.data(),
                   ip.data()};
        references = {z.size(), z.data()};
    }
};

struct Outputs {
    std::array<CWave, 8> v;
    std::vector<double> power;
    double residual = 911.;
    rfmodel_conversion_output output{};
    rfmodel_conversion_loaded_output loaded{};

    explicit Outputs(size_t n) : power(n + 1, 911.) {
        for (size_t i = 0; i < v.size(); ++i) {
            v[i].assign((i < 2 ? n : n * n) + 1, sentinel);
        }
        output = {v[0].data(), v[1].data(), v[2].data(), v[3].data(), n + 1, n * n + 1, &residual};
        loaded = {
            v[4].data(), v[5].data(), v[6].data(), v[7].data(), power.data(), n * n + 1, n + 1};
    }

    void check(const ConversionResult &result) const {
        equal(v[0], result.incident);
        equal(v[1], result.outgoing);
        equal(v[2], result.outgoing_noise.covariance.values);
        equal(v[3], result.outgoing_noise.complementary.values);
        equal(v[4], result.incident_noise.covariance.values);
        equal(v[5], result.incident_noise.complementary.values);
        equal(v[6], result.incident_outgoing_noise.covariance.values);
        equal(v[7], result.incident_outgoing_noise.complementary.values);
        for (size_t i = 0; i < result.incident.size(); ++i) {
            require(std::abs(power[i] - result.net_noise_into_device_w_per_hz[i]) < 1e-12,
                    "loaded power differs");
        }
        for (const auto &values : v) {
            near(value(values.back()), value(sentinel));
        }
        require(power.back() == 911. && residual < 1e-12, "tail or residual differs");
    }

    template <class F> void failure(F f) {
        const auto saved = v;
        const auto saved_power = power;
        const auto saved_residual = residual;
        require(f() != RFMODEL_OK, "expected rejected C API call");
        for (size_t i = 0; i < v.size(); ++i) {
            require(identical(v[i], saved[i]), "partial output write");
        }
        require(power == saved_power && residual == saved_residual, "partial scalar write");
    }
};

static ConversionNoise rank_noise(const Wave &v, double proper = 0.) {
    auto c = conversion_detail::zero(v.size()), p = c;
    for (size_t i = 0; i < v.size(); ++i) {
        for (size_t j = 0; j < v.size(); ++j) {
            c(i, j) = v[i] * std::conj(v[j]);
            p(i, j) = v[i] * v[j];
        }
        if (i != 0) {
            c(i, i) += proper;
        }
    }
    return {c, p};
}

static void model_boundary_helpers() {
    const size_t n = 3;
    auto a = conversion_detail::zero(n), b = a;
    a(0, 0) = .2;
    a(1, 1) = {.2, .1};
    a(1, 2) = {.08, -.03};
    a(2, 1) = {.3, .2};
    a(2, 2) = .1;
    b(1, 1) = {.04, .02};
    b(1, 2) = {.03, -.01};
    b(2, 1) = {-.04, .06};
    const FrequencyConversionModel model(
        1e6, {{0, 0}, {0, 1}, {1, 2}}, a, b, Wave{50., {31., 12.}, {73., -21.}});
    const ConversionDevice native{model,
                                  {1., {.3, -.2}, {.1, .4}},
                                  {.1, {.1, .04}, {-.2, .1}},
                                  rank_noise({.2, {.3, .1}, {-.1, .2}}, .1),
                                  rank_noise({.1, {-.05, .2}, {.1, -.04}}, .05)};
    EncodedDevice input(native);
    const Wave new_refs{90., {20., -5.}, {88., 11.}}, emission{.13, {.02, -.03}, {-.1, .05}};
    auto new_z = encode(new_refs), old_offset = encode(emission);
    const rfmodel_conversion_references target{n, new_z.data()};
    const rfmodel_conversion_affine_offset offset{n, old_offset.data()};
    std::array<CWave, 7> transformed;
    for (size_t i = 0; i < transformed.size(); ++i) {
        transformed[i].assign((i == 4 ? n : n * n) + 1, sentinel);
    }
    rfmodel_conversion_renormalization_output output{transformed[0].data(),
                                                     transformed[1].data(),
                                                     transformed[2].data(),
                                                     transformed[3].data(),
                                                     transformed[4].data(),
                                                     transformed[5].data(),
                                                     transformed[6].data(),
                                                     n + 1,
                                                     n * n + 1};
    ok(rfmodel_conversion_renormalize_power_waves(
        &input.request, &input.references, &target, &offset, &output));
    const auto change = renormalize_power_waves(model, new_refs);
    const auto noise = renormalize_conversion_noise(change, native.intrinsic_noise);
    equal(transformed[0], change.model.direct().values);
    equal(transformed[1], change.model.conjugate().values);
    equal(transformed[2], change.noise_direct.values);
    equal(transformed[3], change.noise_conjugate.values);
    equal(transformed[4], renormalize_conversion_offset(change, emission));
    equal(transformed[5], noise.covariance.values);
    equal(transformed[6], noise.complementary.values);
    for (const auto &v : transformed) {
        near(value(v.back()), value(sentinel));
    }
    const size_t external[]{0, 1, 2};
    std::array<CWave, 4> boundary;
    for (size_t i = 0; i < boundary.size(); ++i) {
        boundary[i].assign((i < 2 ? n : n * n) + 1, sentinel);
    }
    rfmodel_conversion_boundary_output bo{boundary[0].data(),
                                          boundary[1].data(),
                                          boundary[2].data(),
                                          boundary[3].data(),
                                          n + 1,
                                          n * n + 1};
    ok(rfmodel_conversion_renormalize_boundary_power_waves(
        &input.request, &input.references, &target, external, n, &bo));
    const auto expected_boundary = renormalize_conversion_boundary(
        model, new_refs, {0, 1, 2}, native.source, native.reflection, native.source_noise);
    equal(boundary[0], expected_boundary.source);
    equal(boundary[1], expected_boundary.reflection);
    equal(boundary[2], expected_boundary.source_noise.covariance.values);
    equal(boundary[3], expected_boundary.source_noise.complementary.values);
    for (const auto &v : boundary) {
        near(value(v.back()), value(sentinel));
    }

    Outputs original(n), changed(n);
    ok(rfmodel_conversion_analyze_power_waves(
        &input.request, &input.references, &offset, nullptr, &original.output, &original.loaded));
    auto request = input.request;
    request.direct = transformed[0].data();
    request.conjugate = transformed[1].data();
    request.intrinsic_covariance = transformed[5].data();
    request.intrinsic_complementary = transformed[6].data();
    request.source = boundary[0].data();
    request.reflection = boundary[1].data();
    request.source_covariance = boundary[2].data();
    request.source_complementary = boundary[3].data();
    const rfmodel_conversion_affine_offset changed_offset{n, transformed[4].data()};
    ok(rfmodel_conversion_analyze_power_waves(
        &request, &target, &changed_offset, nullptr, &changed.output, &changed.loaded));
    for (size_t i = 0; i < n; ++i) {
        const auto zo = model.reference_impedances_ohms()[i], zn = new_refs[i];
        const auto ao = value(original.v[0][i]), an = value(changed.v[0][i]);
        const auto old_b = value(original.v[1][i]), new_b = value(changed.v[1][i]);
        near((std::conj(zo) * ao + zo * old_b) / std::sqrt(zo.real()),
             (std::conj(zn) * an + zn * new_b) / std::sqrt(zn.real()));
        near((ao - old_b) / std::sqrt(zo.real()), (an - new_b) / std::sqrt(zn.real()));
        require(std::abs(original.power[i] - changed.power[i]) < 1e-12,
                "noise net power not invariant");
    }
    // Every declared capacity, including tails, participates in overlap rejection.
    for (int scenario = 0; scenario < 14; ++scenario) {
        auto out = output;
        auto old = input.references, next = target;
        auto off = offset;
        auto r = input.request;
        CWave invalid_z = new_z;
        switch (scenario) {
        case 0:
            next.count = n - 1;
            break;
        case 1:
            next.values = nullptr;
            break;
        case 2:
            invalid_z[0].imag = 1.;
            next.values = invalid_z.data();
            break;
        case 3:
            invalid_z[1].real = -1.;
            next.values = invalid_z.data();
            break;
        case 4:
            out.direct = new_z.data();
            break;
        case 5:
            out.conjugate = reinterpret_cast<rfmodel_complex *>(&next);
            break;
        case 6:
            out.offset = old_offset.data();
            break;
        case 7:
            out.noise_direct = out.direct;
            break;
        case 8:
            out.matrix_capacity = std::numeric_limits<size_t>::max();
            break;
        case 9:
            out.wave_capacity = n - 1;
            break;
        case 10:
            off.count = n - 1;
            break;
        case 11:
            out.intrinsic_covariance = nullptr;
            break;
        case 12:
            out.intrinsic_complementary = input.ic.data();
            break;
        case 13:
            old.count = 0;
            break;
        }
        const auto saved = transformed;
        require(rfmodel_conversion_renormalize_power_waves(&r, &old, &next, &off, &out) !=
                    RFMODEL_OK,
                "renormalization invalid call accepted");
        for (size_t i = 0; i < saved.size(); ++i) {
            require(identical(saved[i], transformed[i]), "renorm partial write");
        }
    }
    for (int scenario = 0; scenario < 11; ++scenario) {
        auto out = bo;
        auto ref = target;
        size_t selected[]{0, 1, 2};
        size_t count = n;
        const size_t *channels = selected;
        switch (scenario) {
        case 0:
            selected[1] = 0;
            break;
        case 1:
            selected[1] = n;
            break;
        case 2:
            count = n - 1;
            break; // Nonzero unselected boundary.
        case 3:
            channels = nullptr;
            break;
        case 4:
            out.source = input.source.data();
            break;
        case 5:
            out.reflection = reinterpret_cast<rfmodel_complex *>(selected);
            break;
        case 6:
            out.source_covariance = out.source_complementary;
            break;
        case 7:
            out.matrix_capacity = n * n - 1;
            break;
        case 8:
            out.wave_capacity = std::numeric_limits<size_t>::max();
            break;
        case 9:
            ref.count = n - 1;
            break;
        case 10:
            out.source = reinterpret_cast<rfmodel_complex *>(&out);
            break;
        }
        const auto saved = boundary;
        require(rfmodel_conversion_renormalize_boundary_power_waves(
                    &input.request, &input.references, &ref, channels, count, &out) != RFMODEL_OK,
                "boundary invalid call accepted");
        for (size_t i = 0; i < saved.size(); ++i) {
            require(identical(saved[i], boundary[i]), "boundary partial write");
        }
    }
    // Only the unused output tail overlaps a reference array: it is still rejected.
    {
        CWave storage(2 * n, sentinel);
        std::copy(new_z.begin(), new_z.end(), storage.begin() + n);
        const rfmodel_conversion_references ref{n, storage.data() + n};
        const auto saved = storage;
        auto out = output;
        out.offset = storage.data();
        require(rfmodel_conversion_renormalize_power_waves(
                    &input.request, &input.references, &ref, &offset, &out) != RFMODEL_OK,
                "renormalization reference in output tail accepted");
        require(identical(storage, saved), "renormalization tail alias mutated input");
        auto bound = bo;
        bound.source = storage.data();
        require(rfmodel_conversion_renormalize_boundary_power_waves(
                    &input.request, &input.references, &ref, external, n, &bound) != RFMODEL_OK,
                "boundary reference in output tail accepted");
        require(identical(storage, saved), "boundary tail alias mutated input");
    }
    // Reference descriptors themselves are protected even in a matrix capacity tail.
    {
        struct Storage {
            rfmodel_complex matrix[9];
            rfmodel_conversion_references reference;
        } storage{};

        static_assert(offsetof(Storage, reference) == 9 * sizeof(rfmodel_complex),
                      "unexpected padding");
        storage.reference = target;
        const auto saved = storage;
        auto out = output;
        out.direct = storage.matrix;
        require(rfmodel_conversion_renormalize_power_waves(
                    &input.request, &input.references, &storage.reference, &offset, &out) !=
                    RFMODEL_OK,
                "reference descriptor in matrix output tail accepted");
        require(std::memcmp(&storage, &saved, sizeof(storage)) == 0,
                "reference descriptor tail changed");
        auto bound = bo;
        bound.source_covariance = storage.matrix;
        require(rfmodel_conversion_renormalize_boundary_power_waves(
                    &input.request, &input.references, &storage.reference, external, n, &bound) !=
                    RFMODEL_OK,
                "boundary reference descriptor in matrix tail accepted");
        require(std::memcmp(&storage, &saved, sizeof(storage)) == 0,
                "boundary reference descriptor tail changed");
    }
    // Identical references preserve original A/B representation, including DC columns.
    ok(rfmodel_conversion_renormalize_power_waves(
        &input.request, &input.references, &input.references, nullptr, &output));
    equal(transformed[0], model.direct().values);
    equal(transformed[1], model.conjugate().values);
    equal(transformed[4], Wave(n));
    // An empty external selection is valid only for an entirely zero boundary.
    auto silent = input.request;
    silent.source = silent.reflection = silent.source_covariance = silent.source_complementary =
        nullptr;
    ok(rfmodel_conversion_renormalize_boundary_power_waves(
        &silent, &input.references, &target, nullptr, 0, &bo));
    equal(boundary[0], Wave(n));
    equal(boundary[2], Wave(n * n));
}

static void network_and_noise() {
    auto a = conversion_detail::zero(2), b = a;
    a(0, 0) = .1;
    a(0, 1) = {.2, .04};
    a(1, 0) = {.7, -.03};
    a(1, 1) = -.1;
    b(0, 1) = {.01, -.02};
    b(1, 0) = {.02, .01};
    const FrequencyConversionModel m1(1e6, {{0, 1}, {1, 1}}, a, b, Wave{{31., 12.}, {73., -21.}});
    const FrequencyConversionModel m2(1e6, {{0, 1}, {1, 1}}, a, b, Wave{{42., -9.}, {100., 17.}});
    const auto zero = m1.zero_noise();
    auto sn1 = zero, sn2 = zero, intrinsic = zero;
    sn1.covariance(0, 0) = .3;
    sn1.complementary(0, 0) = {.1, .04};
    sn2.covariance(1, 1) = .2;
    sn2.complementary(1, 1) = {-.03, .08};
    intrinsic.covariance(0, 0) = .1;
    intrinsic.covariance(1, 1) = .2;
    intrinsic.complementary(0, 1) = intrinsic.complementary(1, 0) = .04;
    ConversionDevice d1{m1, {{.4, .2}, 0.}, {{.2, .05}, 0.}, sn1, intrinsic};
    ConversionDevice d2{m2, {0., {-.1, .2}}, {0., {-.15, .06}}, sn2, intrinsic};
    EncodedDevice c1(d1), c2(d2);
    rfmodel_conversion_request devices[]{c1.request, c2.request};
    rfmodel_conversion_references refs[]{c1.references, c2.references};
    const rfmodel_conversion_connection connection{0, 1, 1, 0};
    const auto extra = rank_noise({{.2, .1}, 0., 0., {-.1, .3}});
    auto ec = encode(extra.covariance.values), ep = encode(extra.complementary.values);
    const rfmodel_conversion_source_noise additional{4, ec.data(), ep.data()};
    const Wave emission{{.01, .02}, {-.02, .03}, {.04, -.01}, {.02, .02}};
    auto offset_values = encode(emission);
    const rfmodel_conversion_affine_offset offset{4, offset_values.data()};
    FrequencyConversionNetwork native(1e6);
    native.add_power_waves(d1);
    native.add_power_waves(d2);
    native.connect(0, 1, 1, 0);
    Outputs out(4);
    ok(rfmodel_conversion_network_analyze_power_waves(
        devices, 2, &connection, 1, refs, &offset, &additional, &out.output, &out.loaded));
    out.check(native.analyze(true, &extra, emission));
    const auto z1 = value(c1.z[1]), z2 = value(c2.z[0]);
    const auto ai = value(out.v[0][1]), aj = value(out.v[0][2]);
    const auto bi = value(out.v[1][1]), bj = value(out.v[1][2]);
    near((std::conj(z1) * ai + z1 * bi) / std::sqrt(z1.real()),
         (std::conj(z2) * aj + z2 * bj) / std::sqrt(z2.real()));
    near((ai - bi) / std::sqrt(z1.real()), -(aj - bj) / std::sqrt(z2.real()));
    require(std::abs(out.power[1] + out.power[2]) < 1e-12, "wire noise power not conserved");
    for (int scenario = 0; scenario < 11; ++scenario) {
        auto r = additional;
        auto o = out.output;
        auto l = out.loaded;
        auto z = c2.references;
        auto saved_refs = refs[1];
        auto saved_connection = connection;
        switch (scenario) {
        case 0:
            r.count = 3;
            break;
        case 1:
            o.incident = ec.data();
            break;
        case 2:
            o.outgoing = reinterpret_cast<rfmodel_complex *>(&r);
            break;
        case 3:
            l.incident_covariance = c2.z.data();
            break;
        case 4:
            l.incident_complementary = reinterpret_cast<rfmodel_complex *>(refs);
            break;
        case 5:
            l.net_noise_into_device_w_per_hz = reinterpret_cast<double *>(&l);
            break;
        case 6:
            l.matrix_capacity = 15;
            break;
        case 7:
            o.noise_complementary = reinterpret_cast<rfmodel_complex *>(&saved_connection);
            break;
        case 8:
            z.count = 1;
            refs[1] = z;
            break;
        case 9:
            saved_connection.second_port = 7;
            break;
        case 10:
            devices[1].spacing_hz = 2e6;
            break;
        }
        out.failure([&] {
            return rfmodel_conversion_network_analyze_power_waves(
                devices, 2, &saved_connection, 1, refs, &offset, &r, &o, &l);
        });
        refs[1] = saved_refs;
        devices[1].spacing_hz = 1e6;
    }
    // Reference-temperature experiment retains vector coordinates and intrinsic C/P.
    const size_t reference[]{0};
    const rfmodel_conversion_noise_request request{reference, 1, reference, 1, 3, 290.};
    rfmodel_conversion_noise_result result{};
    ok(rfmodel_conversion_network_noise_analysis_power_waves(
        devices, 2, &connection, 1, refs, &request, &result));
    const auto expected = native.reference_noise_analysis({0}, {0}, 3, 290.);
    require(std::abs(result.reference_gain - expected.reference_gain) < 1e-12,
            "reference gain differs");
    require(result.output_noise_w_per_hz == expected.output_noise_w_per_hz &&
                result.reference_output_noise_w_per_hz ==
                    expected.reference_output_noise_w_per_hz &&
                result.noise_factor == expected.noise_factor &&
                result.noise_figure_db == expected.noise_figure_db &&
                result.equivalent_input_temperature_k == expected.equivalent_input_temperature_k,
            "noise experiment differs");
    // The final scalar bytes overlap a reference array or descriptor, never the
    // start of the result. Both partial-overlap shapes must remain failure-atomic.
    {
        struct Storage {
            double prefix[5];
            rfmodel_complex z[2];
        } storage{};

        storage.z[0] = c1.z[0];
        storage.z[1] = c1.z[1];
        const auto saved_storage = storage;
        auto r0 = refs[0];
        refs[0].values = storage.z;
        require(rfmodel_conversion_network_noise_analysis_power_waves(
                    devices,
                    2,
                    &connection,
                    1,
                    refs,
                    &request,
                    reinterpret_cast<rfmodel_conversion_noise_result *>(&storage)) != RFMODEL_OK,
                "noise result tail/reference array overlap accepted");
        require(std::memcmp(&storage, &saved_storage, sizeof(storage)) == 0,
                "noise tail array changed");
        refs[0] = r0;

        struct DescriptorStorage {
            double prefix[5];
            rfmodel_conversion_references references[2];
        } descriptors{};

        descriptors.references[0] = refs[0];
        descriptors.references[1] = refs[1];
        const auto saved_descriptors = descriptors;
        require(rfmodel_conversion_network_noise_analysis_power_waves(
                    devices,
                    2,
                    &connection,
                    1,
                    descriptors.references,
                    &request,
                    reinterpret_cast<rfmodel_conversion_noise_result *>(&descriptors)) !=
                    RFMODEL_OK,
                "noise result tail/reference descriptor overlap accepted");
        require(std::memcmp(&descriptors, &saved_descriptors, sizeof(descriptors)) == 0,
                "noise tail descriptor changed");
    }
    const auto saved = result;
    require(rfmodel_conversion_network_noise_analysis_power_waves(
                devices, 2, &connection, 1, nullptr, &request, &result) != RFMODEL_OK,
            "missing noise refs accepted");
    require(std::memcmp(&saved, &result, sizeof(result)) == 0, "noise result partial write");
    require(rfmodel_conversion_network_noise_analysis_power_waves(
                devices,
                2,
                &connection,
                1,
                refs,
                &request,
                reinterpret_cast<rfmodel_conversion_noise_result *>(refs)) != RFMODEL_OK,
            "noise descriptor alias accepted");
}

static void wide_references_and_singular_helpers() {
    const FrequencyConversionModel model(1., {{0, 1}}, {1, {0.}}, {1, {0.}}, 1.);
    const ConversionNoise noise{{1, {1.}}, {1, {Complex{.3, .2}}}};
    EncodedDevice input({model, {1.}, {0.}, noise, noise});
    const rfmodel_complex one{1., 0.};
    const rfmodel_conversion_affine_offset offset{1, &one};
    std::array<rfmodel_complex, 7> transformed;
    const rfmodel_conversion_renormalization_output out{&transformed[0],
                                                        &transformed[1],
                                                        &transformed[2],
                                                        &transformed[3],
                                                        &transformed[4],
                                                        &transformed[5],
                                                        &transformed[6],
                                                        1,
                                                        1};
    std::array<rfmodel_complex, 4> boundary;
    const rfmodel_conversion_boundary_output bo{
        &boundary[0], &boundary[1], &boundary[2], &boundary[3], 1, 1};
    const size_t external = 0;
    const auto relative = [](rfmodel_complex actual, Complex expected) {
        require(std::abs(value(actual) - expected) < 1e-12 * std::abs(expected),
                "wide-reference C ABI precision lost");
    };
    for (auto z : Wave{1e-24, {1e-24, 3e-25}, 1e-12, 1e12, 1e24, {1e24, 3e23}}) {
        const rfmodel_complex ref{z.real(), z.imag()};
        const rfmodel_conversion_references target{1, &ref};
        ok(rfmodel_conversion_renormalize_power_waves(
            &input.request, &input.references, &target, &offset, &out));
        ok(rfmodel_conversion_renormalize_boundary_power_waves(
            &input.request, &input.references, &target, &external, 1, &bo));
        // Independent Thevenin/Norton scalar transfer, including full improper C/P.
        const auto device_transfer = 2. * std::sqrt(z.real()) / (1. + z);
        const auto boundary_transfer = 2. * std::sqrt(z.real()) / (1. + std::conj(z));
        relative(transformed[2], device_transfer);
        require(std::abs(value(transformed[3])) < 1e-12 * std::abs(device_transfer),
                "spurious conjugate transfer");
        relative(transformed[4], device_transfer);
        relative(transformed[5], std::norm(device_transfer));
        relative(transformed[6], device_transfer * device_transfer * Complex{.3, .2});
        relative(boundary[0], boundary_transfer);
        relative(boundary[2], std::norm(boundary_transfer));
        relative(boundary[3], boundary_transfer * boundary_transfer * Complex{.3, .2});
    }
    rfmodel_complex target_value{9., 0.};
    rfmodel_conversion_references target{1, &target_value};
    input.a[0] = {1.25, 0.};
    const auto saved_model = transformed;
    require(rfmodel_conversion_renormalize_power_waves(
                &input.request, &input.references, &target, &offset, &out) != RFMODEL_OK,
            "singular model conversion accepted");
    require(std::memcmp(transformed.data(), saved_model.data(), sizeof(transformed)) == 0,
            "singular model conversion partially wrote");
    input.a[0] = {0., 0.};
    target_value = {4., 0.};
    input.reflection[0] = {5. / 3., 0.};
    const auto saved_boundary = boundary;
    require(rfmodel_conversion_renormalize_boundary_power_waves(
                &input.request, &input.references, &target, &external, 1, &bo) != RFMODEL_OK,
            "singular boundary conversion accepted");
    require(std::memcmp(boundary.data(), saved_boundary.data(), sizeof(boundary)) == 0,
            "singular boundary conversion partially wrote");
    // 50 -> 100 with Gamma=3 is exactly singular in physical coordinates, but
    // separately rounded coordinate terms leave a spurious tiny denominator.
    // Nonzero source and improper C/P must not be amplified into a fake result.
    for (double scale : {1e-12, 1., 1e12}) {
        input.z[0] = {50. * scale, 0.};
        target_value = {100. * scale, 0.};
        input.reflection[0] = {3., 0.};
        const auto saved = boundary;
        require(rfmodel_conversion_renormalize_boundary_power_waves(
                    &input.request, &input.references, &target, &external, 1, &bo) != RFMODEL_OK,
                "cancellation-singular boundary conversion accepted");
        require(std::memcmp(boundary.data(), saved.data(), sizeof(boundary)) == 0,
                "cancellation-singular boundary conversion partially wrote");
        input.reflection[0] = {3. + 1e-8, 0.};
        ok(rfmodel_conversion_renormalize_boundary_power_waves(
            &input.request, &input.references, &target, &external, 1, &bo));
    }
}

int main() {
    try {
        model_boundary_helpers();
        network_and_noise();
        wide_references_and_singular_helpers();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
