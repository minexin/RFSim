#include "rfmodel/c_api.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            fprintf(                                                                               \
                stderr, "C API check failed at line %d: %s\n", __LINE__, rfmodel_last_error());    \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(int argc, char **argv) {
    {
        const size_t ports[2] = {0, 1};
        const int bins[2] = {1, 1};
        rfmodel_complex direct[4] = {{0., 0.}, {.5, 0.}, {.5, 0.}, {0., 0.}};
        const rfmodel_complex zero[4] = {{0., 0.}};
        const rfmodel_complex source[2] = {{1., 0.}, {0., 0.}};
        rfmodel_complex values[40], saved[40];
        rfmodel_conversion_request devices[2] = {{0}};
        rfmodel_conversion_connection wires[2] = {{0, 1, 1, 0}, {0, 1, 1, 0}};
        double residual = 19.;
        rfmodel_conversion_output output = {
            values, values + 4, values + 8, values + 24, 4, 16, &residual};
        size_t i;
        for (i = 0; i < 2; ++i) {
            devices[i].count = 2;
            devices[i].spacing_hz = 1e9;
            devices[i].reference_ohms = 50.;
            devices[i].physical_ports = ports;
            devices[i].bins = bins;
            devices[i].direct = direct;
            devices[i].conjugate = zero;
        }
        devices[0].source = source;
        CHECK(rfmodel_conversion_network_analyze(devices, 2, wires, 1, &output) == RFMODEL_OK);
        CHECK(fabs(values[7].real - .25) < 1e-12);
        CHECK(fabs(values[2].real - .5) < 1e-12);
        {
            size_t reference = 0, thermal = 0;
            rfmodel_conversion_noise_request request = {&reference, 1, &thermal, 1, 3, 290.};
            rfmodel_conversion_noise_result metric, original;
            const double kt = 1.380649e-23 * 290.;
            const rfmodel_complex intrinsic[4] = {
                {.75 * kt, 0.}, {0., 0.}, {0., 0.}, {.75 * kt, 0.}};
            devices[0].intrinsic_covariance = devices[1].intrinsic_covariance = intrinsic;
            CHECK(rfmodel_conversion_network_noise_analysis(
                      devices, 2, wires, 1, &request, &metric) == RFMODEL_OK);
            CHECK(fabs(metric.reference_gain - .0625) < 1e-12 &&
                  fabs(metric.noise_factor - 16.) < 1e-10);
            original = metric;
            reference = 1;
            CHECK(rfmodel_conversion_network_noise_analysis(
                      devices, 2, wires, 1, &request, &metric) != RFMODEL_OK);
            CHECK(memcmp(&metric, &original, sizeof(metric)) == 0);
            reference = 0;
            request.reference_temperature_k = 0.;
            CHECK(rfmodel_conversion_network_noise_analysis(
                      devices, 2, wires, 1, &request, &metric) != RFMODEL_OK);
            CHECK(memcmp(&metric, &original, sizeof(metric)) == 0);
            request.reference_temperature_k = 290.;
            CHECK(rfmodel_conversion_network_noise_analysis(
                      devices,
                      2,
                      wires,
                      1,
                      &request,
                      (rfmodel_conversion_noise_result *)&devices[1]) != RFMODEL_OK);
            CHECK(devices[1].count == 2 && devices[1].spacing_hz == 1e9);
            CHECK(
                rfmodel_conversion_network_noise_analysis(
                    devices, 2, wires, 1, &request, (rfmodel_conversion_noise_result *)&request) !=
                RFMODEL_OK);
            CHECK(request.reference_count == 1 && request.reference_temperature_k == 290.);
            devices[0].intrinsic_covariance = devices[1].intrinsic_covariance = NULL;
        }
        for (i = 0; i < 40; ++i) {
            values[i].real = 101. + (double)i;
            values[i].imag = -17.;
        }
        memcpy(saved, values, sizeof(values));
        residual = 19.;
        /* Duplicate wires, bad grid/reference, and undersized outputs are atomic. */
        CHECK(rfmodel_conversion_network_analyze(devices, 2, wires, 2, &output) != RFMODEL_OK);
        CHECK(memcmp(saved, values, sizeof(values)) == 0 && residual == 19.);
        devices[1].spacing_hz = 2e9;
        CHECK(rfmodel_conversion_network_analyze(devices, 2, wires, 1, &output) != RFMODEL_OK);
        devices[1].spacing_hz = 1e9;
        devices[1].reference_ohms = 75.;
        CHECK(rfmodel_conversion_network_analyze(devices, 2, wires, 1, &output) != RFMODEL_OK);
        devices[1].reference_ohms = 50.;
        output.matrix_capacity = 15;
        CHECK(rfmodel_conversion_network_analyze(devices, 2, wires, 1, &output) != RFMODEL_OK);
        output.matrix_capacity = 16;
        CHECK(memcmp(saved, values, sizeof(values)) == 0 && residual == 19.);
        /* The second request and connection descriptors are also input ranges. */
        output.incident = direct;
        CHECK(rfmodel_conversion_network_analyze(devices, 2, wires, 1, &output) != RFMODEL_OK);
        CHECK(direct[1].real == .5);
        output.incident = (rfmodel_complex *)&wires[0];
        CHECK(rfmodel_conversion_network_analyze(devices, 2, wires, 1, &output) != RFMODEL_OK);
        CHECK(wires[0].second_device == 1 && wires[0].first_port == 1);
        output.incident = values;
        output.outgoing = values;
        CHECK(rfmodel_conversion_network_analyze(devices, 2, wires, 1, &output) != RFMODEL_OK);
        CHECK(memcmp(saved, values, sizeof(values)) == 0 && residual == 19.);
        CHECK(rfmodel_conversion_network_analyze(NULL, 2, wires, 1, &output) != RFMODEL_OK);
    }

    {
        const size_t ports[1] = {0};
        const int bins[1] = {1};
        const rfmodel_complex direct[1] = {{.2, 0.}}, conjugate[1] = {{.1, 0.}};
        const rfmodel_complex source[1] = {{2., 3.}}, reflection[1] = {{.5, 0.}};
        rfmodel_complex outputs[4] = {{7., 8.}, {9., 10.}, {11., 12.}, {13., 14.}}, saved[4];
        double residual = 17.;
        rfmodel_conversion_request request = {0};
        rfmodel_conversion_output result = {
            outputs, outputs + 1, outputs + 2, outputs + 3, 1, 1, &residual};
        request.count = 1;
        request.spacing_hz = 1.;
        request.reference_ohms = 50.;
        request.physical_ports = ports;
        request.bins = bins;
        request.direct = direct;
        request.conjugate = conjugate;
        request.source = source;
        request.reflection = reflection;
        memcpy(saved, outputs, sizeof(outputs));
        result.wave_capacity = 0;
        CHECK(rfmodel_conversion_analyze(&request, &result) != RFMODEL_OK);
        CHECK(memcmp(saved, outputs, sizeof(outputs)) == 0 && residual == 17.);
        result.wave_capacity = 1;
        result.outgoing = outputs;
        CHECK(rfmodel_conversion_analyze(&request, &result) != RFMODEL_OK);
        CHECK(memcmp(saved, outputs, sizeof(outputs)) == 0 && residual == 17.);
        result.outgoing = outputs + 1;
        request.direct = outputs;
        CHECK(rfmodel_conversion_analyze(&request, &result) != RFMODEL_OK);
        CHECK(memcmp(saved, outputs, sizeof(outputs)) == 0 && residual == 17.);
        request.direct = direct;
        request.spacing_hz = -1.;
        CHECK(rfmodel_conversion_analyze(&request, &result) != RFMODEL_OK);
        CHECK(memcmp(saved, outputs, sizeof(outputs)) == 0 && residual == 17.);
        request.spacing_hz = 1.;
        CHECK(rfmodel_conversion_analyze(&request, &result) == RFMODEL_OK);
        CHECK(fabs(outputs[1].real - .6 / .85) < 1e-12 && fabs(outputs[1].imag - .3 / .95) < 1e-12);
        CHECK(outputs[2].real == 0. && residual < 1e-14);
    }
    {
        const size_t ports[3] = {0, 1, 1};
        const int bins[3] = {8, 2, 18};
        rfmodel_complex a[9] = {{7., 8.}}, b[9] = {{9., 10.}}, saved[9];
        memcpy(saved, a, sizeof(a));
        CHECK(rfmodel_ideal_mixer_conversion(1., ports, bins, 3, 10, 0., 0., 0, 1, 50., a, a, 9) !=
              RFMODEL_OK);
        CHECK(memcmp(saved, a, sizeof(a)) == 0);
        CHECK(rfmodel_ideal_mixer_conversion(1., ports, bins, 3, 10, 0., 0., 0, 1, 50., a, b, 9) ==
              RFMODEL_OK);
        CHECK(a[6].real == 1. && b[3].real == 1.);
    }

    {
        rfmodel_chebyshev_parameters p = {RFMODEL_CHEBYSHEV_LOWPASS, 3, 1e9, 0., 1., 1., 1, 50.};
        rfmodel_complex out[4] = {{7., 8.}, {9., 10.}, {11., 12.}, {13., 14.}}, saved[4];
        memcpy(saved, out, sizeof(out));
        CHECK(rfmodel_chebyshev_s(1e9, &p, out, 3) != RFMODEL_OK);
        CHECK(memcmp(out, saved, sizeof(out)) == 0);
        p.input_stopband_open = 2;
        CHECK(rfmodel_chebyshev_s(1e9, &p, out, 4) != RFMODEL_OK);
        CHECK(memcmp(out, saved, sizeof(out)) == 0);
        p.input_stopband_open = 1;
        CHECK(rfmodel_chebyshev_s(1e9, &p, (rfmodel_complex *)&p, 4) != RFMODEL_OK);
        CHECK(p.order == 3);
        CHECK(rfmodel_chebyshev_s(1e9, &p, out, 4) == RFMODEL_OK);
        CHECK(fabs(out[2].real * out[2].real + out[2].imag * out[2].imag - pow(10., -.1)) < 1e-12);
        p.order = 1;
        memcpy(saved, out, sizeof(out));
        CHECK(rfmodel_chebyshev_s(1e9, &p, out, 4) != RFMODEL_OK);
        CHECK(memcmp(out, saved, sizeof(out)) == 0);
    }

    {
        rfmodel_butterworth_parameters p = {
            RFMODEL_BUTTERWORTH_LOWPASS, 3, 1e9, 0., 3.010299956639812, 1, 50.};
        rfmodel_complex out[4] = {{7., 8.}, {9., 10.}, {11., 12.}, {13., 14.}}, saved[4];
        memcpy(saved, out, sizeof(out));
        CHECK(rfmodel_butterworth_s(1e9, &p, out, 3) != RFMODEL_OK);
        CHECK(memcmp(out, saved, sizeof(out)) == 0);
        p.input_stopband_open = 2;
        CHECK(rfmodel_butterworth_s(1e9, &p, out, 4) != RFMODEL_OK);
        CHECK(memcmp(out, saved, sizeof(out)) == 0);
        p.input_stopband_open = 1;
        CHECK(rfmodel_butterworth_s(1e9, &p, (rfmodel_complex *)&p, 4) != RFMODEL_OK);
        CHECK(p.order == 3);
        CHECK(rfmodel_butterworth_s(1e9, &p, out, 4) == RFMODEL_OK);
        CHECK(fabs(out[2].real * out[2].real + out[2].imag * out[2].imag - .5) < 1e-12);
        p.order = 1;
        memcpy(saved, out, sizeof(out));
        CHECK(rfmodel_butterworth_s(1e9, &p, out, 4) != RFMODEL_OK);
        CHECK(memcmp(out, saved, sizeof(out)) == 0);
    }

    {
        rfmodel_complex output[16], saved[16], branches[2] = {{.5, 0.}, {0., .5}};
        size_t i;
        for (i = 0; i < 16; ++i) {
            output[i].real = 7.;
            output[i].imag = 8.;
        }
        memcpy(saved, output, sizeof(output));
        CHECK(rfmodel_ideal_rlc_s(
                  1e9, RFMODEL_IDEAL_RESISTOR, RFMODEL_SERIES_IMPEDANCE, 50., 50., output, 3) !=
              RFMODEL_OK);
        CHECK(memcmp(output, saved, sizeof(output)) == 0);
        CHECK(rfmodel_ideal_rlc_s(
                  1e9, (rfmodel_ideal_element)9, RFMODEL_SERIES_IMPEDANCE, 50., 50., output, 4) !=
              RFMODEL_OK);
        CHECK(memcmp(output, saved, sizeof(output)) == 0);
        CHECK(rfmodel_matched_transmission_s(1e9, -1., 0., 50., output, 4) != RFMODEL_OK);
        CHECK(memcmp(output, saved, sizeof(output)) == 0);
        CHECK(rfmodel_equal_power_divider_s(1e9, 65, 0., 50., output, 16) != RFMODEL_OK);
        CHECK(memcmp(output, saved, sizeof(output)) == 0);
        CHECK(rfmodel_quadrature_coupler_s(1e9, 2., 0., 50., output, 16) != RFMODEL_OK);
        CHECK(memcmp(output, saved, sizeof(output)) == 0);
        CHECK(rfmodel_isolated_power_divider_s(1e9, branches, 2, 50., branches, 9) != RFMODEL_OK);
        CHECK(branches[0].real == .5 && branches[1].imag == .5);
        CHECK(rfmodel_isolated_power_divider_s(1e9, branches, 2, 50., output, 8) != RFMODEL_OK);
        CHECK(memcmp(output, saved, sizeof(output)) == 0);
        CHECK(rfmodel_ideal_rlc_s(
                  1e9, RFMODEL_IDEAL_RESISTOR, RFMODEL_SERIES_IMPEDANCE, 50., 50., output, 4) ==
              RFMODEL_OK);
        CHECK(fabs(output[0].real - 1. / 3.) < 1e-12 && fabs(output[2].real - 2. / 3.) < 1e-12);
        CHECK(rfmodel_matched_transmission_s(1e9, 20., .25e-9, 50., output, 4) == RFMODEL_OK);
        CHECK(fabs(output[2].real) < 1e-12 && fabs(output[2].imag + .1) < 1e-12);
        CHECK(rfmodel_equal_power_divider_s(1e9, 2, 0., 50., output, 9) == RFMODEL_OK);
        CHECK(fabs(output[3].real - 1. / sqrt(2.)) < 1e-12);
        CHECK(rfmodel_isolated_power_divider_s(1e9, branches, 2, 50., output, 9) == RFMODEL_OK);
        CHECK(output[6].imag == .5 && output[2].imag == .5);
        CHECK(rfmodel_quadrature_coupler_s(1e9, .25, 0., 50., output, 16) == RFMODEL_OK);
        CHECK(fabs(output[8].imag - .5) < 1e-12 && output[12].real == 0.);
    }

    {
        const double kt = 1.380649e-23 * 290.;
        rfmodel_complex scattering[4] = {{0., 0.}, {.5, 0.}, {.5, 0.}, {0., 0.}};
        rfmodel_complex refs[2] = {{50., 0.}, {50., 0.}};
        rfmodel_complex noise[4] = {{.75 * kt, 0.}, {0., 0.}, {0., 0.}, {.75 * kt, 0.}};
        rfmodel_complex output[4] = {{7., 8.}, {9., 10.}, {11., 12.}, {13., 14.}}, saved[4];
        rfmodel_complex source = {50., 0.};
        rfmodel_noise_parameters params = {99., {98., 97.}, 96.}, saved_params;
        double nf = 88.;
        memcpy(saved, output, sizeof(output));
        memcpy(&saved_params, &params, sizeof(params));
        CHECK(rfmodel_power_wave_extract_noise_parameters(
                  scattering, 3, noise, refs, 290., &params) != RFMODEL_OK);
        CHECK(memcmp(&params, &saved_params, sizeof(params)) == 0);
        CHECK(rfmodel_power_wave_noise_figure(scattering, 4, noise, refs, source, 0., &nf) !=
              RFMODEL_OK);
        CHECK(nf == 88.);
        CHECK(rfmodel_power_wave_noise_figure(
                  scattering, 4, noise, refs, source, 290., &noise[0].real) != RFMODEL_OK);
        CHECK(noise[0].real == .75 * kt);
        CHECK(rfmodel_power_wave_extract_noise_parameters(
                  scattering, 4, noise, refs, 290., &params) == RFMODEL_OK);
        CHECK(fabs(params.noise_resistance_ohms - 46.875) < 1e-10);
        CHECK(rfmodel_power_wave_noise_figure(scattering, 4, noise, refs, source, 290., &nf) ==
              RFMODEL_OK);
        CHECK(fabs(nf - 10. * log10(4.)) < 1e-12);
        CHECK(rfmodel_power_wave_noise_from_parameters(
                  scattering, 4, &params, refs, 290., output, 3) != RFMODEL_OK);
        CHECK(memcmp(output, saved, sizeof(output)) == 0);
        CHECK(rfmodel_power_wave_noise_from_parameters(
                  scattering, 4, &params, refs, 290., scattering, 4) != RFMODEL_OK);
        CHECK(scattering[1].real == .5);
        CHECK(rfmodel_power_wave_noise_from_parameters(
                  scattering, 4, &params, refs, 290., output, 4) == RFMODEL_OK);
        CHECK(fabs(output[0].real - noise[0].real) < 1e-34);
        CHECK(fabs(output[3].real - noise[3].real) < 1e-34);
        memcpy(saved, output, sizeof(output));
        params.noise_resistance_ohms = -1.;
        CHECK(rfmodel_power_wave_noise_from_parameters(
                  scattering, 4, &params, refs, 290., output, 4) != RFMODEL_OK);
        CHECK(memcmp(output, saved, sizeof(output)) == 0);
    }

    {
        rfmodel_complex s = {0., 0.}, old_reference = {50., 0.}, new_reference = {75., 20.};
        rfmodel_complex covariance = {1., 0.}, output = {17., 18.}, noise = {19., 20.};
        rfmodel_complex saved_output = output, saved_noise = noise, impedance;
        CHECK(rfmodel_power_wave_renormalize(
                  1, &s, 1, &old_reference, &new_reference, &covariance, &output, &noise, 0) !=
              RFMODEL_OK);
        CHECK(memcmp(&output, &saved_output, sizeof(output)) == 0);
        CHECK(memcmp(&noise, &saved_noise, sizeof(noise)) == 0);
        covariance.real = -1.;
        CHECK(rfmodel_power_wave_renormalize(
                  1, &s, 1, &old_reference, &new_reference, &covariance, &output, &noise, 1) !=
              RFMODEL_OK);
        CHECK(memcmp(&output, &saved_output, sizeof(output)) == 0);
        CHECK(memcmp(&noise, &saved_noise, sizeof(noise)) == 0);
        covariance.real = 1.;
        CHECK(rfmodel_power_wave_renormalize(
                  1, &s, 1, &old_reference, &new_reference, &covariance, &output, &output, 1) !=
              RFMODEL_OK);
        CHECK(memcmp(&output, &saved_output, sizeof(output)) == 0);
        CHECK(rfmodel_power_wave_renormalize(
                  1, &s, 1, &old_reference, &new_reference, &covariance, &s, &noise, 1) !=
              RFMODEL_OK);
        CHECK(s.real == 0. && s.imag == 0.);
        CHECK(rfmodel_power_wave_renormalize(
                  1, &s, 1, &old_reference, &new_reference, &covariance, &output, &noise, 1) ==
              RFMODEL_OK);
        CHECK(fabs(output.real + 2725. / 16025.) < 1e-13);
        CHECK(fabs(output.imag - 3000. / 16025.) < 1e-13);
        CHECK(fabs(noise.real - 15000. / 16025.) < 1e-13 && fabs(noise.imag) < 1e-13);
        CHECK(rfmodel_power_wave_s_to_parameters(1, &output, 1, &new_reference, 0, &impedance, 1) ==
              RFMODEL_OK);
        CHECK(fabs(impedance.real - 50.) < 1e-11 && fabs(impedance.imag) < 1e-11);
        CHECK(rfmodel_power_wave_s_to_parameters(1, &output, 1, &new_reference, 1, &impedance, 1) ==
              RFMODEL_OK);
        CHECK(fabs(impedance.real - .02) < 1e-13 && fabs(impedance.imag) < 1e-13);
        CHECK(rfmodel_power_wave_renormalize(
                  1, &s, 1, &old_reference, &old_reference, NULL, &output, NULL, 1) == RFMODEL_OK);
        CHECK(output.real == 0. && output.imag == 0.);
        s.real = 1.;
        saved_output = output;
        CHECK(rfmodel_power_wave_s_to_parameters(1, &s, 1, &old_reference, 0, &output, 1) !=
              RFMODEL_OK);
        CHECK(memcmp(&output, &saved_output, sizeof(output)) == 0);
    }
    {
        double levels[3] = {0., -40., -60.};
        int signs[2] = {1, -1};
        double output[4] = {7., 8., 9., 10.}, saved[4];
        size_t count = 88;
        memcpy(saved, output, sizeof(output));
        CHECK(rfmodel_polynomial_coefficients_from_intermod_levels(
                  10., levels, 3, signs, 2, 50., output, 3, &count) != RFMODEL_OK);
        CHECK(count == 88 && memcmp(output, saved, sizeof(output)) == 0);
        CHECK(rfmodel_polynomial_coefficients_from_intermod_levels(
                  10., levels, 3, signs, 1, 50., output, 4, &count) != RFMODEL_OK);
        CHECK(count == 88 && memcmp(output, saved, sizeof(output)) == 0);
        CHECK(rfmodel_polynomial_coefficients_from_intermod_levels(
                  10., levels, 3, signs, 2, 50., levels, 3, &count) != RFMODEL_OK);
        CHECK(levels[0] == 0. && levels[1] == -40. && levels[2] == -60. && count == 88);
        CHECK(rfmodel_polynomial_coefficients_from_intermod_levels(
                  10., levels, 3, signs, 2, 50., output, 4, (size_t *)output) != RFMODEL_OK);
        CHECK(memcmp(output, saved, sizeof(output)) == 0);
        CHECK(rfmodel_polynomial_coefficients_from_intermod_levels(
                  10., levels, 3, signs, 2, 50., output, 4, &count) == RFMODEL_OK);
        CHECK(count == 4 && output[0] == 0. && output[2] > 0. && output[3] < 0.);
        CHECK(fabs(output[1] / sqrt(10.) - 1.) < 1e-14);
        CHECK(rfmodel_polynomial_coefficients_from_intermod_levels(
                  10., levels, 1, NULL, 0, 50., output, 4, &count) == RFMODEL_OK);
        CHECK(count == 2);
    }
    {
        double coefficients[10] = {0.};
        rfmodel_coherent_component input = {10, RFMODEL_SPECTRUM_SOURCE, 1., 7, {.01, 0.}};
        rfmodel_coherent_component reduced = {0}, saved_reduced;
        rfmodel_coherent_polynomial_term_v2 terms[7] = {{0}}, saved_terms[7];
        rfmodel_amplifier_drive drive = {77., 88.};
        rfmodel_amplifier_operating_point point = {1., 2., 3., 4., 5.}, saved_point;
        size_t reduced_count = 55, term_count = 66;
        coefficients[9] = 1.;
        memcpy(&saved_reduced, &reduced, sizeof(reduced));
        memcpy(saved_terms, terms, sizeof(terms));
        memcpy(&saved_point, &point, sizeof(point));
        CHECK(rfmodel_highorder_amplifier_evaluate(1e8,
                                                   &input,
                                                   1,
                                                   10.,
                                                   20.,
                                                   23.,
                                                   coefficients,
                                                   10,
                                                   50.,
                                                   100,
                                                   0,
                                                   &reduced,
                                                   1,
                                                   &reduced_count,
                                                   terms,
                                                   6,
                                                   &term_count,
                                                   &drive,
                                                   &point) != RFMODEL_OK);
        CHECK(memcmp(&saved_reduced, &reduced, sizeof(reduced)) == 0);
        CHECK(memcmp(saved_terms, terms, sizeof(terms)) == 0);
        CHECK(memcmp(&saved_point, &point, sizeof(point)) == 0);
        CHECK(reduced_count == 55 && term_count == 66 && drive.total_input_power_w == 77.);
        CHECK(rfmodel_highorder_amplifier_evaluate(1e8,
                                                   &input,
                                                   1,
                                                   10.,
                                                   20.,
                                                   23.,
                                                   coefficients,
                                                   10,
                                                   50.,
                                                   100,
                                                   0,
                                                   &reduced,
                                                   1,
                                                   &reduced_count,
                                                   terms,
                                                   7,
                                                   &term_count,
                                                   &drive,
                                                   &point) == RFMODEL_OK);
        CHECK(reduced_count == 1 && term_count == 7 && terms[6].order == 11);
        CHECK(terms[6].input_indices[10] == 1 && terms[6].component.index == 110);
        CHECK(fabs(terms[6].component.amplitude.real / (pow(5., 10.) * pow(.01, 11.)) - 1.) <
              1e-12);
        CHECK(point.nonlinear_input_scale == 1. && reduced.amplitude.real == .01);
        memcpy(&saved_point, &point, sizeof(point));
        CHECK(rfmodel_get_highorder_amplifier_operating_point(
                  -1., 10., 20., 23., coefficients, 10, 50., &point) != RFMODEL_OK);
        CHECK(memcmp(&saved_point, &point, sizeof(point)) == 0);
        CHECK(rfmodel_get_highorder_amplifier_operating_point(
                  0., 10., 20., 23., NULL, 0, 50., &point) == RFMODEL_OK);
        CHECK(point.nonlinear_input_scale == 1. && point.quadratic_voltage_coefficient == 0.);
    }
    {
        /* V1 must retain the historical layout and stride, including in arrays. */
        struct legacy_term_layout {
            int order;
            int input_indices[9];
            rfmodel_coherent_component component;
        };

        struct guarded_legacy_buffer {
            rfmodel_coherent_polynomial_term terms[5];
            unsigned char canary[32];
        } legacy;

        double coefficients[12] = {0.};
        rfmodel_coherent_component input = {10, RFMODEL_SPECTRUM_SOURCE, 1., 7, {.01, 0.}};
        rfmodel_coherent_component reduced, saved_reduced;
        rfmodel_coherent_polynomial_term_v2 terms[6], saved_terms[6];
        size_t reduced_count = 77, term_count = 88, i, j;
        CHECK(sizeof(rfmodel_coherent_polynomial_term) == sizeof(struct legacy_term_layout));
        CHECK(offsetof(rfmodel_coherent_polynomial_term, component) ==
              offsetof(struct legacy_term_layout, component));
        memset(&legacy, 0, sizeof(legacy));
        memset(legacy.canary, 0xA5, sizeof(legacy.canary));
        memset(terms, 0x5A, sizeof(terms));
        memset(&reduced, 0x5A, sizeof(reduced));
        memcpy(saved_terms, terms, sizeof(terms));
        memcpy(&saved_reduced, &reduced, sizeof(reduced));
        coefficients[11] = 1.;
        CHECK(rfmodel_coherent_polynomial_evaluate(1e8,
                                                   &input,
                                                   1,
                                                   coefficients,
                                                   12,
                                                   50.,
                                                   100,
                                                   &reduced,
                                                   1,
                                                   &reduced_count,
                                                   legacy.terms,
                                                   5,
                                                   &term_count) != RFMODEL_OK);
        CHECK(reduced_count == 77 && term_count == 88 && legacy.terms[0].order == 0);
        CHECK(memcmp(&reduced, &saved_reduced, sizeof(reduced)) == 0);
        /* Capacity failure is atomic across both buffers and both counts. */
        CHECK(rfmodel_coherent_polynomial_evaluate_v2(1e8,
                                                      &input,
                                                      1,
                                                      coefficients,
                                                      12,
                                                      50.,
                                                      100,
                                                      &reduced,
                                                      1,
                                                      &reduced_count,
                                                      terms,
                                                      5,
                                                      &term_count) != RFMODEL_OK);
        CHECK(reduced_count == 77 && term_count == 88);
        CHECK(memcmp(terms, saved_terms, sizeof(terms)) == 0);
        CHECK(memcmp(&reduced, &saved_reduced, sizeof(reduced)) == 0);
        CHECK(rfmodel_coherent_polynomial_evaluate_v2(1e8,
                                                      &input,
                                                      1,
                                                      coefficients,
                                                      12,
                                                      50.,
                                                      100,
                                                      &reduced,
                                                      1,
                                                      &reduced_count,
                                                      terms,
                                                      6,
                                                      &term_count) == RFMODEL_OK);
        CHECK(reduced_count == 1 && term_count == 6 && terms[5].order == 11);
        CHECK(terms[5].component.index == 110 && terms[5].input_indices[10] == 1);
        CHECK(fabs(terms[5].component.amplitude.real / (pow(5., 10.) * pow(.01, 11.)) - 1.) <
              1e-12);
        coefficients[11] = 0.;
        coefficients[9] = 1.;
        CHECK(rfmodel_coherent_polynomial_evaluate(1e8,
                                                   &input,
                                                   1,
                                                   coefficients,
                                                   10,
                                                   50.,
                                                   100,
                                                   &reduced,
                                                   1,
                                                   &reduced_count,
                                                   legacy.terms,
                                                   5,
                                                   &term_count) == RFMODEL_OK);
        CHECK(term_count == 5);
        CHECK(rfmodel_coherent_polynomial_evaluate_v2(1e8,
                                                      &input,
                                                      1,
                                                      coefficients,
                                                      10,
                                                      50.,
                                                      100,
                                                      &reduced,
                                                      1,
                                                      &reduced_count,
                                                      terms,
                                                      6,
                                                      &term_count) == RFMODEL_OK);
        CHECK(term_count == 5);
        for (i = 0; i < 5; ++i) {
            CHECK(legacy.terms[i].order == terms[i].order);
            CHECK(legacy.terms[i].component.index == terms[i].component.index);
            CHECK(legacy.terms[i].component.amplitude.real == terms[i].component.amplitude.real);
            CHECK(legacy.terms[i].component.amplitude.imag == terms[i].component.amplitude.imag);
            CHECK(legacy.terms[i].component.coherence_group == terms[i].component.coherence_group);
            for (j = 0; j < 9; ++j) {
                CHECK(legacy.terms[i].input_indices[j] == terms[i].input_indices[j]);
            }
            CHECK(terms[i].input_indices[9] == 0 && terms[i].input_indices[10] == 0);
        }
        for (i = 0; i < sizeof(legacy.canary); ++i) {
            CHECK(legacy.canary[i] == 0xA5);
        }
    }

    {
        rfmodel_two_tone_intercept intercept = {3, -1, 33., 1, RFMODEL_INTERCEPT_OUTPUT};
        double coefficients[10];
        double original[10];
        size_t count = 99;
        size_t i;
        for (i = 0; i < 10; ++i) {
            coefficients[i] = 123. + (double)i;
        }
        memcpy(original, coefficients, sizeof(original));
        CHECK(rfmodel_polynomial_coefficients_from_intercepts(
                  10., &intercept, 1, 50., coefficients, 4, &count) != RFMODEL_OK);
        CHECK(count == 99 && memcmp(original, coefficients, sizeof(original)) == 0);
        intercept.coefficient_sign = 0;
        CHECK(rfmodel_polynomial_coefficients_from_intercepts(
                  10., &intercept, 1, 50., coefficients, 10, &count) != RFMODEL_OK);
        CHECK(count == 99 && memcmp(original, coefficients, sizeof(original)) == 0);
        intercept.coefficient_sign = 1;
        CHECK(rfmodel_polynomial_coefficients_from_intercepts(
                  10., &intercept, 1, 50., coefficients, 10, NULL) != RFMODEL_OK);
        CHECK(rfmodel_polynomial_coefficients_from_intercepts(
                  10., NULL, 1, 50., coefficients, 10, &count) != RFMODEL_OK);
        CHECK(rfmodel_polynomial_coefficients_from_intercepts(
                  10., &intercept, 1, 50., coefficients, 10, (size_t *)coefficients) != RFMODEL_OK);
        CHECK(memcmp(original, coefficients, sizeof(original)) == 0);
        CHECK(rfmodel_polynomial_coefficients_from_intercepts(
                  10., &intercept, 1, 50., (double *)&intercept, 3, &count) != RFMODEL_OK);
        CHECK(intercept.intercept_dbm == 33. && count == 99);
        CHECK(rfmodel_polynomial_coefficients_from_intercepts(
                  10., &intercept, 1, 50., coefficients, 10, &count) == RFMODEL_OK);
        CHECK(count == 5 && coefficients[0] == 0. && coefficients[2] == 0. &&
              coefficients[3] == 0.);
        CHECK(fabs(coefficients[4] - .07096267784671509) < 1e-14);
        CHECK(coefficients[5] == original[5]);
        CHECK(rfmodel_polynomial_coefficients_from_intercepts(
                  0., NULL, 0, 50., coefficients, 2, &count) == RFMODEL_OK);
        CHECK(count == 2 && coefficients[0] == 0. && coefficients[1] == 1.);
    }

    {
        double gain = 99.;
        rfmodel_amplifier_operating_point point = {11., 22., 33., 44., 55.};
        if (rfmodel_saturating_amplitude_gain(-1., 20., 20., 23., &gain) == RFMODEL_OK ||
            gain != 99. ||
            rfmodel_get_amplifier_operating_point(-1., 20., 20., 23., 20., 10., 50., &point) ==
                RFMODEL_OK ||
            point.fundamental_amplitude_gain != 11. || point.nonlinear_input_scale != 22. ||
            point.limited_input_power_w != 33. || point.quadratic_voltage_coefficient != 44. ||
            point.cubic_voltage_coefficient != 55.) {
            return 79;
        }
        if (rfmodel_saturating_amplitude_gain(0., 20., 20., 23., &gain) != RFMODEL_OK ||
            gain != 10. ||
            rfmodel_get_amplifier_operating_point(0., 20., 20., 23., 20., 10., 50., &point) !=
                RFMODEL_OK ||
            point.fundamental_amplitude_gain != 10. || point.nonlinear_input_scale != 1. ||
            point.limited_input_power_w != 0.) {
            return 80;
        }
        if (rfmodel_saturating_amplitude_gain(0., 20., 20., 23., NULL) == RFMODEL_OK ||
            rfmodel_get_amplifier_operating_point(0., 20., 20., 23., 20., 10., 50., NULL) ==
                RFMODEL_OK) {
            return 81;
        }
    }

    {
        rfmodel_origin_factor root = {0, 1};
        rfmodel_origin_contribution contribution = {&root, 1, {1., 0.}};
        rfmodel_origin_expression parent = {&contribution, 1};
        rfmodel_origin_expression_term term = {99, 88, {77., 66.}};
        rfmodel_origin_factor factor = {55, -1};
        size_t terms = 44, factors = 33;
        rfmodel_complex total = {22., 11.};
        /* Invalid provenance and insufficient term capacity must preserve outputs. */
        CHECK(rfmodel_sum_origin_expressions(
                  &parent, 1, &term, 1, &terms, &factor, 1, &factors, &total) != RFMODEL_OK);
        CHECK(term.factor_offset == 99 && term.amplitude.real == 77. && factor.root_id == 55 &&
              terms == 44 && factors == 33 && total.real == 22.);
        root.root_id = 7;
        CHECK(rfmodel_sum_origin_expressions(
                  &parent, 1, &term, 0, &terms, &factor, 1, &factors, &total) != RFMODEL_OK);
        CHECK(term.factor_offset == 99 && term.amplitude.real == 77. && factor.root_id == 55 &&
              terms == 44 && factors == 33 && total.real == 22.);
        CHECK(rfmodel_sum_origin_expressions(
                  NULL, 1, &term, 1, &terms, &factor, 1, &factors, &total) != RFMODEL_OK);
        /* Each input has finite power, but their coherent total overflows power. */
        {
            const rfmodel_origin_factor roots[] = {{7, 1}, {9, 1}};
            const rfmodel_origin_contribution inputs[] = {{roots, 1, {1e154, 0.}},
                                                          {roots + 1, 1, {1e154, 0.}}};
            const rfmodel_origin_expression expression = {inputs, 2};
            rfmodel_origin_expression_term output_terms[2] = {{99, 88, {77., 66.}}};
            rfmodel_origin_factor output_factors[2] = {{55, -1}};
            CHECK(
                rfmodel_sum_origin_expressions(
                    &expression, 1, output_terms, 2, &terms, output_factors, 2, &factors, &total) !=
                RFMODEL_OK);
            CHECK(output_terms[0].factor_offset == 99 && output_terms[0].amplitude.real == 77. &&
                  output_factors[0].root_id == 55 && terms == 44 && factors == 33 &&
                  total.real == 22. && total.imag == 11.);
        }
    }

    {
        const rfmodel_origin_factor a[] = {{7, 1}}, b[] = {{9, 1}};
        const rfmodel_origin_contribution contributions[] = {{a, 1, {1., 0.}}, {b, 1, {-1., 0.}}};
        const rfmodel_origin_expression parent = {contributions, 2};
        const int indices[] = {1, 1};
        rfmodel_origin_expression_term terms[3] = {{99, 88, {77., 66.}}};
        rfmodel_origin_factor factors[6] = {{99, -1}};
        size_t term_count = 55, factor_count = 44;
        rfmodel_complex total = {33., 22.};
        if (rfmodel_product_origin_expressions(&parent,
                                               1,
                                               indices,
                                               2,
                                               (rfmodel_complex){1., 0.},
                                               terms,
                                               3,
                                               &term_count,
                                               factors,
                                               5,
                                               &factor_count,
                                               &total) == RFMODEL_OK ||
            terms[0].factor_offset != 99 || terms[0].factor_count != 88 ||
            terms[0].amplitude.real != 77. || terms[0].amplitude.imag != 66. ||
            factors[0].root_id != 99 || factors[0].sign != -1 || term_count != 55 ||
            factor_count != 44 || total.real != 33. || total.imag != 22.) {
            return 75;
        }
        if (rfmodel_product_origin_expressions(&parent,
                                               1,
                                               indices,
                                               2,
                                               (rfmodel_complex){1., 0.},
                                               terms,
                                               3,
                                               &term_count,
                                               factors,
                                               6,
                                               &factor_count,
                                               &total) != RFMODEL_OK ||
            term_count != 3 || factor_count != 6 || total.real != 0. || total.imag != 0. ||
            terms[1].amplitude.real != -2. || terms[1].factor_offset != 2 ||
            factors[2].root_id != 7 || factors[3].root_id != 9) {
            return 76;
        }
        if (rfmodel_sum_origin_expressions(
                &parent, 1, terms, 3, &term_count, factors, 6, &factor_count, &total) !=
                RFMODEL_OK ||
            term_count != 2 || factor_count != 2 || total.real != 0.) {
            return 77;
        }
        if (rfmodel_sum_origin_expressions(
                NULL, 0, NULL, 0, &term_count, NULL, 0, &factor_count, &total) != RFMODEL_OK ||
            term_count != 0 || factor_count != 0 || total.real != 0. || total.imag != 0.) {
            return 78;
        }
    }

    {
        const rfmodel_origin_factor a[] = {{7, 1}};
        const rfmodel_origin_factor b[] = {{9, -1}, {7, 1}};
        const rfmodel_mixing_origin parents[] = {{a, 1}, {b, 2}};
        const int indices[] = {2, -1};
        rfmodel_origin_factor output[3] = {{99, 1}};
        size_t count = 99;
        if (rfmodel_expand_mixing_origin(parents, 2, indices, 2, output, 1, &count) == RFMODEL_OK ||
            count != 99 || output[0].root_id != 99) {
            return 73;
        }
        if (rfmodel_expand_mixing_origin(parents, 2, indices, 2, output, 3, &count) != RFMODEL_OK ||
            count != 3 || output[0].root_id != 7 || output[0].sign != -1 ||
            output[1].root_id != 7 || output[1].sign != 1 || output[2].root_id != 9 ||
            output[2].sign != -1) {
            return 74;
        }
    }

    {
        const double coefficients[10] = {0., 0., 0., 0., 0., 0., 0., 0., 0., 1.};
        rfmodel_coherent_component input = {10, RFMODEL_SPECTRUM_HARMONIC, 1., 7, {.01, 0.}};
        rfmodel_coherent_component reduced = {99, 0, 1., 1, {0., 0.}};
        rfmodel_coherent_polynomial_term terms[5] = {{0}};
        size_t reduced_count = 77, term_count = 88;
        terms[0].order = 99;
        if (rfmodel_coherent_polynomial_evaluate(1e8,
                                                 &input,
                                                 1,
                                                 coefficients,
                                                 10,
                                                 50.,
                                                 100,
                                                 &reduced,
                                                 1,
                                                 &reduced_count,
                                                 terms,
                                                 4,
                                                 &term_count) == RFMODEL_OK ||
            reduced.index != 99 || reduced_count != 77 || term_count != 88 ||
            terms[0].order != 99) {
            return 71;
        }
        if (rfmodel_coherent_polynomial_evaluate(1e8,
                                                 &input,
                                                 1,
                                                 coefficients,
                                                 10,
                                                 50.,
                                                 100,
                                                 &reduced,
                                                 1,
                                                 &reduced_count,
                                                 terms,
                                                 5,
                                                 &term_count) != RFMODEL_OK ||
            reduced_count != 1 || term_count != 5 || terms[4].order != 9 ||
            terms[4].component.index != 90 || terms[4].input_indices[8] != 1 ||
            terms[4].component.coherence_group <= 100) {
            return 72;
        }
    }

    rfmodel_network *network = NULL;
    const rfmodel_complex pad[4] = {{0, 0}, {0.5, 0}, {0.5, 0}, {0, 0}};
    const rfmodel_complex zero = {0, 0}, one = {1, 0};
    rfmodel_complex incident[4], outgoing[4], scattering[4];
    size_t offset = 999, count = 0;
    const size_t external[2] = {0, 3};
    double residual = -1;
    CHECK(rfmodel_abi_version() == 1);
    {
        rfmodel_coherent_component input[] = {{10, RFMODEL_SPECTRUM_SOURCE, 1., 7, {.01, 0.}},
                                              {10, RFMODEL_SPECTRUM_SOURCE, 1., 9, {0., .02}}};
        rfmodel_coherent_component reduced[2], saved_reduced[2];
        rfmodel_coherent_amplifier_term terms[15], saved_terms[15];
        rfmodel_amplifier_drive drive = {97., 96.};
        size_t reduced_count = 99, term_count = 98;
        int failure;
        memset(reduced, 0, sizeof(reduced));
        memset(terms, 0, sizeof(terms));
        reduced[0].index = 999;
        terms[0].order = 999;
        memcpy(saved_reduced, reduced, sizeof(reduced));
        memcpy(saved_terms, terms, sizeof(terms));
        for (failure = 0; failure < 5; ++failure) {
            CHECK(rfmodel_coherent_amplifier_evaluate(1e8,
                                                      input,
                                                      2,
                                                      20.,
                                                      20.,
                                                      23.,
                                                      20.,
                                                      10.,
                                                      50.,
                                                      failure == 4 ? UINT64_MAX : 100,
                                                      reduced,
                                                      failure == 0 ? 1 : 2,
                                                      failure == 2 ? NULL : &reduced_count,
                                                      terms,
                                                      failure == 1 ? 14 : 15,
                                                      &term_count,
                                                      failure == 3 ? NULL : &drive) != RFMODEL_OK);
            CHECK(memcmp(reduced, saved_reduced, sizeof(reduced)) == 0);
            CHECK(memcmp(terms, saved_terms, sizeof(terms)) == 0);
            CHECK(reduced_count == 99 && term_count == 98);
            CHECK(drive.total_input_power_w == 97. && drive.limited_input_power_w == 96.);
        }
        CHECK(rfmodel_coherent_amplifier_evaluate(1e8,
                                                  input,
                                                  2,
                                                  20.,
                                                  20.,
                                                  23.,
                                                  20.,
                                                  10.,
                                                  50.,
                                                  100,
                                                  reduced,
                                                  2,
                                                  &reduced_count,
                                                  terms,
                                                  15,
                                                  &term_count,
                                                  &drive) == RFMODEL_OK);
        CHECK(reduced_count == 2 && term_count == 15);
        input[1].kind = RFMODEL_SPECTRUM_HARMONIC;
        CHECK(rfmodel_coherent_amplifier_cascade(1e8,
                                                 input,
                                                 2,
                                                 20.,
                                                 20.,
                                                 23.,
                                                 20.,
                                                 10.,
                                                 50.,
                                                 100,
                                                 reduced,
                                                 2,
                                                 &reduced_count,
                                                 terms,
                                                 15,
                                                 &term_count,
                                                 &drive) == RFMODEL_OK);
        CHECK(reduced_count == 2 && term_count == 5);
        CHECK(terms[1].component.kind == RFMODEL_SPECTRUM_HARMONIC);
        input[1].kind = RFMODEL_SPECTRUM_SOURCE;
        CHECK(rfmodel_coherent_amplifier_evaluate(1e8,
                                                  input,
                                                  2,
                                                  20.,
                                                  20.,
                                                  23.,
                                                  20.,
                                                  10.,
                                                  50.,
                                                  100,
                                                  reduced,
                                                  2,
                                                  &reduced_count,
                                                  terms,
                                                  15,
                                                  &term_count,
                                                  &drive) == RFMODEL_OK);

        CHECK(fabs(drive.total_input_power_w - .0005) < 1e-15);
        CHECK(terms[0].input_indices[0] == 1 && terms[1].input_indices[0] == 2);
        CHECK(terms[2].component.coherence_group > 100);
        CHECK(rfmodel_coherent_amplifier_evaluate(1e8,
                                                  NULL,
                                                  0,
                                                  20.,
                                                  20.,
                                                  23.,
                                                  20.,
                                                  10.,
                                                  50.,
                                                  0,
                                                  NULL,
                                                  0,
                                                  &reduced_count,
                                                  NULL,
                                                  0,
                                                  &term_count,
                                                  &drive) == RFMODEL_OK);
        CHECK(reduced_count == 0 && term_count == 0 && drive.total_input_power_w == 0.);
    }

    {
        rfmodel_coherent_component input[] = {{10, RFMODEL_SPECTRUM_SOURCE, 1., 7, {.001, 0.}},
                                              {10, RFMODEL_SPECTRUM_SOURCE, 1., 7, {-.001, 0.}}};
        rfmodel_coherent_component groups[2] = {{99, 0, 9., 9, {9., 0.}}}, before[2];
        rfmodel_bin_power powers[2] = {{99, 9.}}, before_powers[2];
        size_t group_count = 99, power_count = 98;
        double output_power = 97., drive = 96.;
        memcpy(before, groups, sizeof(groups));
        memcpy(before_powers, powers, sizeof(powers));
        CHECK(rfmodel_compress_coherent_fundamentals(1e8,
                                                     20.,
                                                     20.,
                                                     23.,
                                                     input,
                                                     2,
                                                     groups,
                                                     2,
                                                     &group_count,
                                                     powers,
                                                     0,
                                                     &power_count,
                                                     &output_power,
                                                     &drive) == RFMODEL_INVALID_ARGUMENT);
        CHECK(group_count == 99 && power_count == 98 && output_power == 97. && drive == 96.);
        CHECK(memcmp(groups, before, sizeof(groups)) == 0 &&
              memcmp(powers, before_powers, sizeof(powers)) == 0);
        input[1].kind = RFMODEL_SPECTRUM_INTERMOD;
        CHECK(rfmodel_compress_coherent_fundamentals(1e8,
                                                     20.,
                                                     20.,
                                                     23.,
                                                     input,
                                                     2,
                                                     groups,
                                                     2,
                                                     &group_count,
                                                     powers,
                                                     2,
                                                     &power_count,
                                                     &output_power,
                                                     &drive) == RFMODEL_INVALID_ARGUMENT);
        CHECK(group_count == 99 && power_count == 98 && output_power == 97. && drive == 96.);
        CHECK(memcmp(groups, before, sizeof(groups)) == 0 &&
              memcmp(powers, before_powers, sizeof(powers)) == 0);
        input[1].kind = RFMODEL_SPECTRUM_SOURCE;
        CHECK(rfmodel_compress_coherent_fundamentals(1e8,
                                                     20.,
                                                     20.,
                                                     23.,
                                                     input,
                                                     2,
                                                     groups,
                                                     2,
                                                     &group_count,
                                                     powers,
                                                     2,
                                                     &power_count,
                                                     &output_power,
                                                     NULL) == RFMODEL_INVALID_ARGUMENT);
        CHECK(group_count == 99 && power_count == 98 && output_power == 97. && drive == 96.);
        CHECK(rfmodel_compress_coherent_fundamentals(1e8,
                                                     20.,
                                                     20.,
                                                     23.,
                                                     input,
                                                     2,
                                                     groups,
                                                     2,
                                                     &group_count,
                                                     powers,
                                                     2,
                                                     &power_count,
                                                     &output_power,
                                                     &drive) == RFMODEL_OK);
        CHECK(group_count == 1 && power_count == 1 && output_power == 0. && drive == 0.);
        CHECK(groups[0].coherence_group == 7 && groups[0].amplitude.real == 0.);
        CHECK(rfmodel_compress_coherent_fundamentals(1e8,
                                                     20.,
                                                     20.,
                                                     23.,
                                                     NULL,
                                                     0,
                                                     NULL,
                                                     0,
                                                     &group_count,
                                                     NULL,
                                                     0,
                                                     &power_count,
                                                     &output_power,
                                                     &drive) == RFMODEL_OK);
        CHECK(group_count == 0 && power_count == 0 && output_power == 0. && drive == 0.);
        CHECK(rfmodel_compress_coherent_fundamentals(1e8,
                                                     20.,
                                                     20.,
                                                     20.,
                                                     NULL,
                                                     0,
                                                     NULL,
                                                     0,
                                                     &group_count,
                                                     NULL,
                                                     0,
                                                     &power_count,
                                                     &output_power,
                                                     &drive) == RFMODEL_INVALID_ARGUMENT);
    }
    {
        rfmodel_coherent_mixer_input input[] = {
            {{10, RFMODEL_SPECTRUM_SOURCE, 1., 7, {1., 0.}}, 8, 0., 0., 9},
            {{10, RFMODEL_SPECTRUM_SOURCE, 1., 7, {0., 1.}}, 8, 0., 1.5707963267948966, 9}};
        rfmodel_coherent_component output[4] = {{99, 0, 9., 9, {9., 0.}}};
        rfmodel_coherent_component before[4];
        size_t produced = 99;
        memcpy(before, output, sizeof(output));
        CHECK(rfmodel_mix_coherent_components(1e8, input, 2, 100, output, 3, &produced) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(produced == 99 && memcmp(before, output, sizeof(output)) == 0);
        input[1].lo_index = 10;
        CHECK(rfmodel_mix_coherent_components(1e8, input, 2, 100, output, 4, &produced) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(produced == 99 && memcmp(before, output, sizeof(output)) == 0);
        input[1].lo_index = 8;
        CHECK(rfmodel_mix_coherent_components(1e8, input, 2, UINT64_MAX, output, 4, &produced) ==
              RFMODEL_SOLVER_ERROR);
        CHECK(produced == 99 && memcmp(before, output, sizeof(output)) == 0);
        CHECK(rfmodel_mix_coherent_components(1e8, input, 2, 100, output, 4, &produced) ==
              RFMODEL_OK);
        CHECK(produced == 4 && output[0].index == 2 && output[1].index == 18);
        CHECK(output[0].coherence_group == output[3].coherence_group &&
              output[0].coherence_group > 100);
        CHECK(fabs(output[2].amplitude.real - 1.) < 1e-12 &&
              fabs(output[3].amplitude.real + 1.) < 1e-12);
        CHECK(rfmodel_mix_coherent_components(1., NULL, 0, 0, NULL, 0, &produced) == RFMODEL_OK);
        CHECK(produced == 0);
        CHECK(rfmodel_mix_coherent_components(1., NULL, 0, 0, NULL, 0, NULL) ==
              RFMODEL_INVALID_ARGUMENT);
    }
    {
        rfmodel_source_coherence sources[] = {{"a", "clock"}, {"b", "clock"}, {"clock", NULL}};
        uint64_t groups[3] = {777, 888, 999};
        CHECK(rfmodel_assign_source_coherence(sources, 3, groups, 2) == RFMODEL_INVALID_ARGUMENT);
        CHECK(groups[0] == 777 && groups[1] == 888 && groups[2] == 999);
        sources[2].source_id = "a";
        CHECK(rfmodel_assign_source_coherence(sources, 3, groups, 3) == RFMODEL_INVALID_ARGUMENT);
        CHECK(groups[0] == 777 && groups[1] == 888 && groups[2] == 999);
        sources[2].source_id = NULL;
        CHECK(rfmodel_assign_source_coherence(sources, 3, groups, 3) == RFMODEL_INVALID_ARGUMENT);
        CHECK(groups[0] == 777 && groups[1] == 888 && groups[2] == 999);
        sources[2].source_id = "clock";
        CHECK(rfmodel_assign_source_coherence(sources, 3, groups, 3) == RFMODEL_OK);
        CHECK(groups[0] == groups[1] && groups[0] != groups[2] && groups[2] != 0);
        CHECK(rfmodel_assign_source_coherence(NULL, 0, NULL, 0) == RFMODEL_OK);
    }
    {
        rfmodel_network *combiner = NULL;
        const rfmodel_complex matrix[] = {
            {0, 0}, {0, 0}, {.5, 0}, {0, 0}, {0, 0}, {.5, 0}, {.5, 0}, {.5, 0}, {0, 0}};
        const size_t ports[] = {2, 1, 0};
        rfmodel_port_coherent_component input[] = {
            {0, {10, RFMODEL_SPECTRUM_SOURCE, 1., 7, {1., 0.}}},
            {1, {10, RFMODEL_SPECTRUM_SOURCE, 1., 7, {-1., 0.}}}};
        rfmodel_coherent_component groups[2] = {{99, 0, 9., 9, {9., 0.}}};
        rfmodel_bin_power powers[2] = {{99, 9.}};
        size_t group_count = 777, power_count = 888, first;
        double total = 999.;
        CHECK(rfmodel_network_create(50., &combiner) == RFMODEL_OK);
        CHECK(rfmodel_network_add(combiner, 3, matrix, 9, 50., &first) == RFMODEL_OK);
        CHECK(rfmodel_network_transmit_coherent(combiner,
                                                ports,
                                                3,
                                                2,
                                                1e8,
                                                input,
                                                2,
                                                groups,
                                                2,
                                                &group_count,
                                                powers,
                                                0,
                                                &power_count,
                                                &total) == RFMODEL_INVALID_ARGUMENT);
        CHECK(group_count == 777 && power_count == 888 && total == 999.);
        CHECK(groups[0].index == 99 && powers[0].index == 99);
        CHECK(rfmodel_network_transmit_coherent(combiner,
                                                ports,
                                                3,
                                                2,
                                                1e8,
                                                input,
                                                2,
                                                groups,
                                                0,
                                                &group_count,
                                                powers,
                                                2,
                                                &power_count,
                                                &total) == RFMODEL_INVALID_ARGUMENT);
        CHECK(group_count == 777 && power_count == 888 && total == 999.);
        CHECK(groups[0].index == 99 && powers[0].index == 99);
        input[0].input_port = 99;
        CHECK(rfmodel_network_transmit_coherent(combiner,
                                                ports,
                                                3,
                                                2,
                                                1e8,
                                                input,
                                                2,
                                                groups,
                                                2,
                                                &group_count,
                                                powers,
                                                2,
                                                &power_count,
                                                &total) == RFMODEL_INVALID_ARGUMENT);
        CHECK(group_count == 777 && powers[0].index == 99 && total == 999.);
        input[0].input_port = 0;
        CHECK(rfmodel_network_transmit_coherent(combiner,
                                                ports,
                                                3,
                                                2,
                                                1e8,
                                                input,
                                                2,
                                                groups,
                                                2,
                                                &group_count,
                                                powers,
                                                2,
                                                &power_count,
                                                &total) == RFMODEL_OK);
        CHECK(group_count == 1 && power_count == 1 && total == 0.);
        CHECK(groups[0].coherence_group == 7 && groups[0].amplitude.real == 0.);
        input[1].component.coherence_group = 8;
        CHECK(rfmodel_network_transmit_coherent(combiner,
                                                ports,
                                                3,
                                                2,
                                                1e8,
                                                input,
                                                2,
                                                groups,
                                                2,
                                                &group_count,
                                                powers,
                                                2,
                                                &power_count,
                                                &total) == RFMODEL_OK);
        CHECK(group_count == 2 && power_count == 1 && total == .5);
        CHECK(rfmodel_network_transmit_coherent(combiner,
                                                ports,
                                                3,
                                                2,
                                                1e8,
                                                NULL,
                                                0,
                                                NULL,
                                                0,
                                                &group_count,
                                                NULL,
                                                0,
                                                &power_count,
                                                &total) == RFMODEL_OK);
        CHECK(group_count == 0 && power_count == 0 && total == 0.);
        rfmodel_network_destroy(combiner);
    }
    {
        const rfmodel_coherent_component input[] = {
            {10, RFMODEL_SPECTRUM_SOURCE, 1., 1, {1., 0.}},
            {10, RFMODEL_SPECTRUM_SOURCE, 1., 1, {-1., 0.}},
            {10, RFMODEL_SPECTRUM_INTERMOD, 1., 1, {1., 0.}}};
        rfmodel_coherent_component output[3] = {{99, 0, 9., 9, {9., 0.}}};
        rfmodel_bin_power powers[3] = {{99, 9.}};
        size_t group_count = 777, power_count = 888;
        double total = 999.;
        CHECK(rfmodel_reduce_coherent_components(
                  1e8, input, 3, output, 3, &group_count, powers, 0, &power_count, &total) !=
              RFMODEL_OK);
        CHECK(group_count == 777 && power_count == 888 && total == 999.);
        CHECK(output[0].index == 99 && powers[0].index == 99);
        CHECK(rfmodel_reduce_coherent_components(
                  1e8, input, 3, output, 3, &group_count, powers, 3, &power_count, &total) ==
              RFMODEL_OK);
        CHECK(group_count == 2 && power_count == 1 && total == 1.);
        CHECK(output[0].amplitude.real == 0. && powers[0].power_w == 1.);
        CHECK(rfmodel_reduce_coherent_components(
                  1e8, NULL, 0, NULL, 0, &group_count, NULL, 0, &power_count, &total) ==
              RFMODEL_OK);
        CHECK(group_count == 0 && power_count == 0 && total == 0.);
    }

    {
        rfmodel_network *term_network = NULL;
        rfmodel_amplifier_term terms[] = {{3, 10, {-11, 10, 11}, {1., 0.}},
                                          {3, 10, {-10, 10, 10}, {-1., 0.}}};
        rfmodel_amplifier_term output[2] = {{99, 98, {97, 0, 0}, {123., 0.}}};
        const size_t ports[] = {0, 1};
        size_t written = 777, first;
        CHECK(rfmodel_network_create(50., &term_network) == RFMODEL_OK);
        CHECK(rfmodel_network_add(term_network, 2, pad, 4, 50., &first) == RFMODEL_OK);
        CHECK(rfmodel_network_transmit_terms(
                  term_network, ports, 2, 1e8, terms, 2, output, 1, &written) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(written == 777 && output[0].order == 99 && output[0].amplitude.real == 123.);
        terms[1].contributors[0] = -9;
        CHECK(rfmodel_network_transmit_terms(
                  term_network, ports, 2, 1e8, terms, 2, output, 2, &written) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(written == 777 && output[0].order == 99);
        terms[1].contributors[0] = -10;
        CHECK(rfmodel_network_transmit_terms(
                  term_network, ports, 2, 1e8, terms, 2, output, 2, &written) == RFMODEL_OK);
        CHECK(written == 2 && output[0].contributors[0] == -11 && output[1].contributors[0] == -10);
        CHECK(output[0].amplitude.real == .5 && output[1].amplitude.real == -.5);
        rfmodel_network_destroy(term_network);
    }
    {
        const rfmodel_spectrum_bin input[] = {{10, {.001, 0.}}, {11, {.001, 0.}}};
        rfmodel_amplifier_term terms[32] = {{99, 98, {97, 96, 95}, {123., 456.}}};
        rfmodel_amplifier_drive drive = {321., 654.};
        size_t written = 777;
        CHECK(rfmodel_multitone_amplifier_terms(
                  1e8, input, 2, 20., 20., 23., 20., 10., 50., terms, 15, &written, &drive) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(written == 777 && drive.total_input_power_w == 321. && terms[0].order == 99 &&
              terms[0].contributors[2] == 95 && terms[0].amplitude.real == 123.);
        CHECK(rfmodel_multitone_amplifier_terms(
                  1e8, input, 2, 20., 20., 23., 20., 10., 50., terms, 32, &written, &drive) ==
              RFMODEL_OK);
        CHECK(written == 16 && terms[7].order == 3 && terms[7].index == 10);
        CHECK(terms[7].contributors[0] == -11 && terms[7].contributors[1] == 10 &&
              terms[7].contributors[2] == 11);
        CHECK(fabs(terms[7].amplitude.real / -2e-6 - 1.) < 1e-12);
        CHECK(terms[8].contributors[0] == -10 &&
              fabs(terms[8].amplitude.real / -1e-6 - 1.) < 1e-12);
        CHECK(rfmodel_multitone_amplifier_terms(
                  1e8, NULL, 0, 20., 20., 23., 20., 10., 50., NULL, 0, &written, &drive) ==
              RFMODEL_OK);
        CHECK(written == 0 && drive.total_input_power_w == 0.);
    }
    {
        const rfmodel_spectrum_bin input[] = {{10, {.001, 0.}}, {11, {.001, 0.}}};
        rfmodel_amplifier_component output[32] = {{99, 98, {123., 456.}}};
        rfmodel_amplifier_drive drive = {321., 654.};
        size_t written = 777;
        CHECK(rfmodel_multitone_amplifier_evaluate(
                  1e8, input, 2, 20., 20., 23., 20., 10., 50., output, 1, &written, &drive) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(written == 777 && drive.total_input_power_w == 321. &&
              drive.limited_input_power_w == 654.);
        CHECK(output[0].order == 99 && output[0].index == 98 && output[0].amplitude.real == 123.);
        CHECK(rfmodel_multitone_amplifier_evaluate(
                  1e8, input, 2, 20., 20., 23., 20., 10., 50., output, 32, &written, NULL) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(written == 777 && output[0].order == 99);
        CHECK(rfmodel_multitone_amplifier_evaluate(
                  1e8, input, 2, 20., 20., 23., 20., 10., 50., output, 32, &written, &drive) ==
              RFMODEL_OK);
        CHECK(written == 14 && fabs(drive.total_input_power_w / 2e-6 - 1.) < 1e-12);
        CHECK(output[2].order == 2 && output[2].index == 1);
        CHECK(fabs(output[2].amplitude.real * output[2].amplitude.real / 1e-9 - 1.) < 1e-12);
        CHECK(output[6].order == 3 && output[6].index == 9);
        CHECK(fabs(output[6].amplitude.real / -1e-6 - 1.) < 1e-12);
        CHECK(rfmodel_multitone_amplifier_evaluate(
                  1e8, NULL, 0, 20., 20., 23., 20., 10., 50., NULL, 0, &written, &drive) ==
              RFMODEL_OK);
        CHECK(written == 0 && drive.total_input_power_w == 0. && drive.limited_input_power_w == 0.);
    }
    {
        const rfmodel_spectrum_bin input[] = {{10, {.001, 0.}}, {11, {.001, 0.}}};
        rfmodel_spectrum_bin output[3] = {{99, {123., 0.}}};
        size_t written = 777;
        CHECK(rfmodel_single_tone_amplifier_transmit(
                  1e6, input, 1, 20., 20., 23., 20., 10., 50., output, 2, &written) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(written == 777 && output[0].index == 99 && output[0].amplitude.real == 123.);
        CHECK(rfmodel_single_tone_amplifier_transmit(
                  1e6, input, 2, 20., 20., 23., 20., 10., 50., output, 3, &written) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(output[0].index == 99);
        CHECK(rfmodel_single_tone_amplifier_transmit(
                  1e6, input, 1, 20., 20., 23., 20., 10., 50., output, 3, &written) == RFMODEL_OK);
        CHECK(written == 3 && output[0].index == 10 && output[1].index == 20 &&
              output[2].index == 30);
        CHECK(fabs(output[1].amplitude.real * output[1].amplitude.real / 2.5e-10 - 1.) < 1e-12);
    }
    {
        rfmodel_complex output = {123., 456.};
        const rfmodel_complex input = {0., .1};
        CHECK(rfmodel_saturating_fundamental(20., 20., 20., input, .01, &output) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(output.real == 123. && output.imag == 456.);
        CHECK(rfmodel_saturating_fundamental(20., 20., 23., input, .001, &output) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(output.real == 123. && output.imag == 456.);
        CHECK(rfmodel_saturating_fundamental(20., 20., 23., input, .01, NULL) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(rfmodel_saturating_fundamental(20., 20., 23., input, .01, &output) == RFMODEL_OK);
        CHECK(output.real == 0. &&
              fabs(output.imag * output.imag / .19922937036162172 - 1.) < 1e-12);
    }
    {
        const rfmodel_spectrum_bin input = {10, {.1, 0.}};
        const double coefficients[] = {0., 0., 1.};
        rfmodel_spectrum_bin output[2] = {{123, {456., 0.}}, {789, {0., 0.}}};
        size_t written = 999;
        CHECK(rfmodel_polynomial_amplifier_transmit(
                  1e6, &input, 1, coefficients, 3, 50., output, 1, &written) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(output[0].index == 123 && output[0].amplitude.real == 456.);
        CHECK(rfmodel_polynomial_amplifier_transmit(
                  1e6, &input, 1, NULL, 3, 50., output, 2, &written) == RFMODEL_INVALID_ARGUMENT);
        CHECK(rfmodel_polynomial_amplifier_transmit(
                  1e6, &input, 1, coefficients, 3, 50., output, 2, &written) == RFMODEL_OK);
        CHECK(written == 2 && output[0].index == 0 && output[1].index == 20);
        CHECK(fabs(output[0].amplitude.real - sqrt(50.) * .01) < 1e-12);
        CHECK(fabs(output[1].amplitude.real - .05) < 1e-12);
    }
    {
        const rfmodel_spectrum_bin bins[] = {{10, {0., .001}}, {20, {.002, 0.}}};
        const rfmodel_incident_spectrum port = {1e6, bins, 2};
        rfmodel_complex output = {123., 456.};
        double total = 789.;
        CHECK(rfmodel_p1db_spectral_fundamental(20., 10., &port, 1, 1, 10, &output, &total) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(output.real == 123. && output.imag == 456. && total == 789.);
        CHECK(rfmodel_p1db_spectral_fundamental(20., 10., &port, 1, 0, 10, &output, NULL) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(output.real == 123. && output.imag == 456. && total == 789.);
        CHECK(rfmodel_p1db_spectral_fundamental(20., 10., &port, 1, 0, 10, &output, &total) ==
              RFMODEL_OK);
        CHECK(fabs(total / 5e-6 - 1.) < 1e-12);
        CHECK(output.real == 0. && output.imag > 0. && output.imag < .01);
    }
    {
        rfmodel_complex output = {123., 456.};
        const rfmodel_complex too_large = {1., 0.};
        const rfmodel_complex point = {0., sqrt(pow(10., -3.9))};
        CHECK(rfmodel_p1db_fundamental(20., 10., too_large, &output) == RFMODEL_INVALID_ARGUMENT);
        CHECK(output.real == 123. && output.imag == 456.);
        CHECK(rfmodel_p1db_fundamental(20., 10., point, NULL) == RFMODEL_INVALID_ARGUMENT);
        CHECK(rfmodel_p1db_fundamental(20., 10., point, &output) == RFMODEL_OK);
        CHECK(fabs(output.imag - .1) < 1e-12 && output.real == 0.);
        {
            const double total = pow(10., -3.9);
            const rfmodel_complex partial = {0., sqrt(total / 4.)};
            CHECK(rfmodel_p1db_driven_fundamental(20., 10., partial, total, &output) == RFMODEL_OK);
            CHECK(fabs(output.imag - .05) < 1e-12 && output.real == 0.);
            CHECK(rfmodel_p1db_driven_fundamental(20., 10., partial, total / 8., &output) ==
                  RFMODEL_INVALID_ARGUMENT);
            CHECK(fabs(output.imag - .05) < 1e-12 && output.real == 0.);
            CHECK(rfmodel_p1db_driven_fundamental(20., 10., partial, total, NULL) ==
                  RFMODEL_INVALID_ARGUMENT);
        }
    }
    {
        rfmodel_touchstone *model = NULL;
        rfmodel_touchstone_info info;
        rfmodel_complex values[4] = {{123., 0.}};
        CHECK(argc == 3);
        CHECK(rfmodel_touchstone_open(argv[1], 2, &model) == RFMODEL_INVALID_ARGUMENT);
        CHECK(model == NULL);
        CHECK(rfmodel_touchstone_open(argv[1], 0, &model) == RFMODEL_OK);
        CHECK(rfmodel_touchstone_get_info(model, &info) == RFMODEL_OK);
        CHECK(info.ports == 2 && info.reference_ohms == 50. && info.noise_sample_count == 0);
        CHECK(info.minimum_frequency_hz == 1e9 && info.maximum_frequency_hz == 3e9);
        CHECK(rfmodel_touchstone_s(model, 2e9, 50., values, 3) == RFMODEL_INVALID_ARGUMENT);
        CHECK(values[0].real == 123.);
        CHECK(rfmodel_touchstone_s(model, 4e9, 50., values, 4) == RFMODEL_INVALID_ARGUMENT);
        CHECK(values[0].real == 123.);
        CHECK(rfmodel_touchstone_s(model, 2e9, 50., values, 4) == RFMODEL_OK);
        CHECK(fabs(values[2].real - 0.375) < 1e-12);
        rfmodel_touchstone_close(model);
        CHECK(rfmodel_touchstone_open(argv[2], 0, &model) == RFMODEL_OK);
        values[0].real = 123.;
        CHECK(rfmodel_touchstone_noise(model, 2e9, 50., 290., values, 3) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(values[0].real == 123.);
        CHECK(rfmodel_touchstone_noise(model, 2e9, 50., 290., values, 4) == RFMODEL_OK);
        CHECK(fabs(values[0].real / (1.380649e-23 * 290.) - .96) < 1e-12);
        CHECK(fabs(values[1].real / (1.380649e-23 * 290.) + .384) < 1e-12);
        rfmodel_touchstone_close(model);
        rfmodel_touchstone_close(NULL);
    }
    {
        const rfmodel_complex s = {0.2, 0.3}, c = {2., 0.}, e = {3., 0.}, gamma = {-0.1, 0.2};
        rfmodel_complex a = {123., 0.}, b = {456., 0.};
        double net = 789., temperature = 290.;
        CHECK(rfmodel_loaded_noise(1, &s, &c, &e, 1, &gamma, 1, &a, &b, 1, &net, 0) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(a.real == 123. && b.real == 456. && net == 789.);
        CHECK(rfmodel_loaded_noise(1, &s, &c, &e, 1, &gamma, 1, &a, &b, 1, &net, 1) == RFMODEL_OK);
        CHECK(fabs(b.real - 2.39 / 1.1665) < 1e-12);
        CHECK(fabs(a.real - 3.1 / 1.1665) < 1e-12);
        CHECK(fabs(net - (a.real - b.real)) < 1e-12);
        CHECK(rfmodel_thermal_boundary_noise(1, &gamma, &temperature, &a, 1) == RFMODEL_OK);
        CHECK(fabs(a.real / (1.380649e-23 * 290.) - 0.95) < 1e-12);
    }
    {
        rfmodel_spectrum_bin input[2] = {{10, {1., 0.}}, {10, {1., 0.}}};
        rfmodel_spectrum_bin result[16] = {{999, {123., 0.}}};
        size_t produced = 999;
        CHECK(rfmodel_ideal_mixer_transmit(1e6, input, 1, 2, 0., 0., 50., result, 1, &produced) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(produced == 999 && result[0].index == 999 && result[0].amplitude.real == 123.);
        CHECK(rfmodel_ideal_mixer_transmit(1e6, input, 2, 2, 0., 0., 50., result, 16, &produced) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(rfmodel_ideal_mixer_transmit(1e6, input, 1, 2, 0., 0., 50., result, 16, &produced) ==
              RFMODEL_OK);
        CHECK(produced == 2 && result[0].index == 8 && result[1].index == 12);
        CHECK(fabs(result[0].amplitude.real - 1.) < 1e-12);
        input[0].amplitude.real = sqrt(1e-5);
        input[1].index = 13;
        input[1].amplitude.real = sqrt(1e-5);
        CHECK(rfmodel_cubic_amplifier_transmit(
                  1e6, input, 2, 20., 10., 50., result, 16, &produced) == RFMODEL_OK);
        CHECK(result[0].index == 7);
        CHECK(fabs(result[0].amplitude.real * result[0].amplitude.real / 1e-9 - 1.) < 1e-12);
        CHECK(rfmodel_cubic_amplifier_transmit(1e6, NULL, 0, 20., 10., 50., NULL, 0, &produced) ==
              RFMODEL_OK);
        CHECK(produced == 0);
    }
    {
        const rfmodel_complex input_z = {50., 50.}, output_z = {50., -50.};
        const rfmodel_complex singular_z = {-50., 0.};
        CHECK(rfmodel_linear_amplifier_s(
                  1e9, 20., 90., 20., -90., input_z, output_z, 50., scattering, 4) == RFMODEL_OK);
        CHECK(fabs(scattering[0].real - 0.2) < 1e-12);
        CHECK(fabs(scattering[0].imag - 0.4) < 1e-12);
        CHECK(fabs(scattering[2].imag - 10.) < 1e-12);
        CHECK(fabs(scattering[1].imag + 0.1) < 1e-12);
        CHECK(rfmodel_linear_amplifier_s(
                  1e9, 20., 0., 20., 0., singular_z, output_z, 50., scattering, 4) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(fabs(scattering[2].imag - 10.) < 1e-12);
    }
    scattering[0].real = 123.;
    CHECK(rfmodel_transmission_line_s(1e9, 100., 0.25e-9, 0., 50., scattering, 3) ==
          RFMODEL_INVALID_ARGUMENT);
    CHECK(scattering[0].real == 123.);
    CHECK(rfmodel_transmission_line_s(1e9, 100., 0.25e-9, 0., 50., scattering, 4) == RFMODEL_OK);
    CHECK(fabs(scattering[0].real - 0.6) < 1e-12 && fabs(scattering[2].imag + 0.8) < 1e-12);
    CHECK(rfmodel_rlgc_line_s(0., 25., 0., 0., 0., 2., 50., scattering, 4) == RFMODEL_OK);
    CHECK(fabs(scattering[2].real - 2. / 3.) < 1e-12);
    CHECK(rfmodel_network_create(-1, &network) == RFMODEL_INVALID_ARGUMENT);
    CHECK(network == NULL && strlen(rfmodel_last_error()) > 0);
    CHECK(rfmodel_network_create(50, &network) == RFMODEL_OK);
    CHECK(rfmodel_network_add(network, 2, pad, 3, 50, &offset) == RFMODEL_INVALID_ARGUMENT);
    CHECK(offset == 999);
    CHECK(rfmodel_network_add(network, 2, pad, 4, 50, &offset) == RFMODEL_OK && offset == 0);
    CHECK(rfmodel_network_add(network, 2, pad, 4, 75, &offset) == RFMODEL_INVALID_ARGUMENT);
    CHECK(rfmodel_network_port_count(network, &count) == RFMODEL_OK && count == 2);
    CHECK(rfmodel_network_add(network, 2, pad, 4, 50, &offset) == RFMODEL_OK && offset == 2);
    CHECK(rfmodel_network_connect(network, 1, 2) == RFMODEL_OK);
    CHECK(rfmodel_network_external_s(network, external, 2, scattering, 4) == RFMODEL_OK);
    {
        const rfmodel_spectrum_bin input = {7, {0., 2.}};
        rfmodel_spectrum_bin output = {999, {123., 0.}};
        size_t produced = 999;
        CHECK(rfmodel_network_transmit_spectrum(
                  network, external, 2, 1e6, &input, 1, &output, 0, &produced) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(produced == 999 && output.index == 999 && output.amplitude.real == 123.);
        CHECK(rfmodel_network_transmit_spectrum(
                  network, external, 2, 1e6, &input, 1, &output, 1, &produced) == RFMODEL_OK);
        CHECK(produced == 1 && output.index == 7 && fabs(output.amplitude.imag - 0.5) < 1e-12);
        CHECK(rfmodel_network_transmit_spectrum(
                  network, external, 2, 1e6, NULL, 0, NULL, 0, &produced) == RFMODEL_OK);
        CHECK(produced == 0);
        CHECK(rfmodel_network_transmit_spectrum(
                  network, external, 1, 1e6, NULL, 0, NULL, 0, &produced) ==
              RFMODEL_INVALID_ARGUMENT);
    }
    CHECK(fabs(scattering[2].real - 0.25) < 1e-12);
    {
        const double thermal = 1.380649e-23 * 290.;
        rfmodel_complex block[4], intrinsic[16] = {{0, 0}}, result[4] = {{123, 0}};
        size_t i;
        CHECK(rfmodel_passive_noise(2, pad, 4, 290., block, 4) == RFMODEL_OK);
        CHECK(fabs(block[0].real / thermal - 0.75) < 1e-12);
        for (i = 0; i < 4; ++i) {
            intrinsic[i * 4 + i] = block[0];
        }
        CHECK(rfmodel_network_external_noise(network, external, 2, intrinsic, 16, result, 3) ==
              RFMODEL_INVALID_ARGUMENT);
        CHECK(result[0].real == 123);
        CHECK(rfmodel_network_external_noise(network, external, 2, intrinsic, 16, result, 4) ==
              RFMODEL_OK);
        CHECK(fabs(result[3].real / thermal - 0.9375) < 1e-12);
        CHECK(rfmodel_passive_noise(2, pad, 4, -1., block, 4) == RFMODEL_INVALID_ARGUMENT);
    }
    CHECK(rfmodel_network_solve(network, incident, outgoing, 4, &residual) ==
          RFMODEL_INVALID_ARGUMENT);
    CHECK(rfmodel_network_terminate(network, 0, zero, one) == RFMODEL_OK);
    CHECK(rfmodel_network_terminate(network, 3, zero, zero) == RFMODEL_OK);
    CHECK(rfmodel_network_solve(network, incident, outgoing, 3, &residual) ==
          RFMODEL_INVALID_ARGUMENT);
    CHECK(residual == -1);
    CHECK(rfmodel_network_solve(network, incident, outgoing, 4, &residual) == RFMODEL_OK);
    CHECK(fabs(outgoing[3].real - 0.25) < 1e-12 && residual < 1e-12);
    CHECK(rfmodel_last_error()[0] == '\0');
    rfmodel_network_destroy(network);
    rfmodel_network_destroy(NULL);
    CHECK(rfmodel_network_port_count(NULL, &count) == RFMODEL_INVALID_ARGUMENT);
    CHECK(rfmodel_network_create(50, &network) == RFMODEL_OK);
    {
        const rfmodel_complex through[4] = {{0, 0}, {1, 0}, {1, 0}, {0, 0}};
        CHECK(rfmodel_network_add(network, 2, through, 4, 50, &offset) == RFMODEL_OK);
        CHECK(rfmodel_network_connect(network, 0, 1) == RFMODEL_OK);
        CHECK(rfmodel_network_solve(network, incident, outgoing, 4, &residual) ==
              RFMODEL_SOLVER_ERROR);
        CHECK(strlen(rfmodel_last_error()) > 0);
    }
    rfmodel_network_destroy(network);
    return 0;
}
