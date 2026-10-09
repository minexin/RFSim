#include <rfmodel/c_api.h>
#include <math.h>

int main(int argc, char **argv) {
    {
        const double coefficients[] = {0., 0., .01};
        rfmodel_amplifier_operating_point point;
        if (rfmodel_get_highorder_amplifier_operating_point(
                .01, 10., 20., 23., coefficients, 3, 50., &point) != RFMODEL_OK ||
            point.nonlinear_input_scale >= 1.) {
            return 84;
        }
    }
    {
        double coefficients[12] = {0.};
        rfmodel_coherent_component input = {10, RFMODEL_SPECTRUM_SOURCE, 1., 7, {.01, 0.}};
        rfmodel_coherent_component reduced;
        rfmodel_coherent_polynomial_term_v2 terms[6];
        size_t reduced_count = 0, term_count = 0;
        coefficients[11] = 1.;
        if (rfmodel_coherent_polynomial_evaluate_v2(1e8,
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
                                                    &term_count) != RFMODEL_OK ||
            reduced_count != 1 || term_count != 6 || terms[5].input_indices[10] != 1 ||
            terms[5].component.index != 110) {
            return 83;
        }
    }

    {
        const rfmodel_two_tone_intercept entry = {3, -1, 33., 1, RFMODEL_INTERCEPT_OUTPUT};
        double coefficients[10];
        size_t count = 0;
        if (rfmodel_polynomial_coefficients_from_intercepts(
                10., &entry, 1, 50., coefficients, 10, &count) != RFMODEL_OK ||
            count != 5 || fabs(coefficients[4] - .07096267784671509) > 1e-14) {
            return 82;
        }
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

    {
        rfmodel_coherent_component input = {10, RFMODEL_SPECTRUM_SOURCE, 1., 7, {.01, 0.}};
        rfmodel_coherent_component reduced;
        rfmodel_coherent_amplifier_term terms[4];
        rfmodel_amplifier_drive drive;
        size_t reduced_count, term_count;
        if (rfmodel_coherent_amplifier_evaluate(1e8,
                                                &input,
                                                1,
                                                20.,
                                                20.,
                                                23.,
                                                20.,
                                                10.,
                                                50.,
                                                100,
                                                &reduced,
                                                1,
                                                &reduced_count,
                                                terms,
                                                4,
                                                &term_count,
                                                &drive) != RFMODEL_OK ||
            reduced_count != 1 || term_count != 4 || terms[1].component.coherence_group <= 100) {
            return 30;
        }
    }

    {
        rfmodel_coherent_component input = {20, RFMODEL_SPECTRUM_HARMONIC, 2., 7, {.01, 0.}};
        rfmodel_coherent_component reduced;
        rfmodel_coherent_amplifier_term term;
        rfmodel_amplifier_drive drive;
        size_t reduced_count, term_count;
        if (rfmodel_coherent_amplifier_cascade(1e8,
                                               &input,
                                               1,
                                               20.,
                                               20.,
                                               23.,
                                               20.,
                                               10.,
                                               50.,
                                               0,
                                               &reduced,
                                               1,
                                               &reduced_count,
                                               &term,
                                               1,
                                               &term_count,
                                               &drive) != RFMODEL_OK ||
            term_count != 1 || term.component.kind != RFMODEL_SPECTRUM_HARMONIC) {
            return 31;
        }
    }
    {
        const double anchor = pow(10., -2.9);
        rfmodel_coherent_component input[] = {{10, RFMODEL_SPECTRUM_SOURCE, 1., 7, {0., 0.}},
                                              {10, RFMODEL_SPECTRUM_SOURCE, 1., 8, {0., 0.}}};
        rfmodel_coherent_component output[2];
        rfmodel_bin_power powers[2];
        size_t group_count, power_count;
        double power, drive;
        input[0].amplitude.real = sqrt(anchor / 2);
        input[1].amplitude.imag = sqrt(anchor / 2);
        if (rfmodel_compress_coherent_fundamentals(1e8,
                                                   20.,
                                                   20.,
                                                   23.,
                                                   input,
                                                   2,
                                                   output,
                                                   2,
                                                   &group_count,
                                                   powers,
                                                   2,
                                                   &power_count,
                                                   &power,
                                                   &drive) != RFMODEL_OK ||
            group_count != 2 || power_count != 1 || fabs(power - .1) > 1e-12 ||
            fabs(drive - anchor) > 1e-12 || output[1].coherence_group != 8) {
            return 29;
        }
    }
    {
        const rfmodel_coherent_mixer_input input = {
            {3, RFMODEL_SPECTRUM_SOURCE, 1., 7, {0., 1.}}, 8, 0., 1.5707963267948966, 9};
        rfmodel_coherent_component output[2];
        size_t produced = 0;
        if (rfmodel_mix_coherent_components(1., &input, 1, 0, output, 2, &produced) != RFMODEL_OK ||
            produced != 2 || output[0].index != 5 || output[1].index != 11 ||
            fabs(output[0].amplitude.real - 1.) > 1e-12 ||
            fabs(output[1].amplitude.real + 1.) > 1e-12 || output[0].coherence_group <= 9) {
            return 28;
        }
    }
    {
        const rfmodel_source_coherence sources[] = {{"a", "clock"}, {"clock", NULL}};
        uint64_t groups[2];
        if (rfmodel_assign_source_coherence(sources, 2, groups, 2) != RFMODEL_OK ||
            groups[0] == 0 || groups[1] == 0 || groups[0] == groups[1]) {
            return 27;
        }
    }
    {
        const rfmodel_coherent_component input[] = {
            {10, RFMODEL_SPECTRUM_SOURCE, 1., 1, {1., 0.}},
            {10, RFMODEL_SPECTRUM_SOURCE, 1., 2, {-1., 0.}}};
        rfmodel_coherent_component groups[2];
        rfmodel_bin_power powers[2];
        size_t group_count, power_count;
        double total;
        if (rfmodel_reduce_coherent_components(
                1e8, input, 2, groups, 2, &group_count, powers, 2, &power_count, &total) !=
                RFMODEL_OK ||
            group_count != 2 || power_count != 1 || total != 2.) {
            return 25;
        }
    }
    rfmodel_network *network = NULL;
    const rfmodel_complex s[4] = {{0, 0}, {0, -0.5}, {0, -0.5}, {0, 0}};
    const size_t ports[2] = {0, 1};
    rfmodel_complex result[4];
    size_t offset;
    int status;
    {
        rfmodel_network *coherent_network = NULL;
        const rfmodel_port_coherent_component input = {
            0, {10, RFMODEL_SPECTRUM_SOURCE, 1., 7, {1., 0.}}};
        rfmodel_coherent_component group;
        rfmodel_bin_power power;
        size_t group_count, power_count, first;
        double total;
        if (rfmodel_network_create(50., &coherent_network) != RFMODEL_OK ||
            rfmodel_network_add(coherent_network, 2, s, 4, 50., &first) != RFMODEL_OK ||
            rfmodel_network_transmit_coherent(coherent_network,
                                              ports,
                                              2,
                                              1,
                                              1e8,
                                              &input,
                                              1,
                                              &group,
                                              1,
                                              &group_count,
                                              &power,
                                              1,
                                              &power_count,
                                              &total) != RFMODEL_OK) {
            rfmodel_network_destroy(coherent_network);
            return 26;
        }
        rfmodel_network_destroy(coherent_network);
        if (group_count != 1 || power_count != 1 || total != .25 || group.coherence_group != 7 ||
            group.amplitude.imag != -.5) {
            return 26;
        }
    }
    {
        rfmodel_network *term_network = NULL;
        const rfmodel_amplifier_term input = {1, 10, {10, 0, 0}, {1., 0.}};
        rfmodel_amplifier_term output;
        size_t written, first;
        if (rfmodel_network_create(50., &term_network) != RFMODEL_OK ||
            rfmodel_network_add(term_network, 2, s, 4, 50., &first) != RFMODEL_OK ||
            rfmodel_network_transmit_terms(
                term_network, ports, 2, 1e8, &input, 1, &output, 1, &written) != RFMODEL_OK) {
            rfmodel_network_destroy(term_network);
            return 22;
        }
        rfmodel_network_destroy(term_network);
        if (written != 1 || output.contributors[0] != 10 ||
            fabs(output.amplitude.imag + .5) > 1e-12) {
            return 22;
        }
    }
    {
        const rfmodel_spectrum_bin input[] = {{10, {.001, 0.}}, {11, {.001, 0.}}};
        rfmodel_amplifier_term terms[32];
        rfmodel_amplifier_drive drive;
        size_t written;
        if (rfmodel_multitone_amplifier_terms(
                1e8, input, 2, 20., 20., 23., 20., 10., 50., terms, 32, &written, &drive) !=
                RFMODEL_OK ||
            written != 16 || terms[7].contributors[0] != -11 ||
            fabs(terms[7].amplitude.real / -2e-6 - 1.) > 1e-12) {
            return 21;
        }
    }
    {
        const rfmodel_spectrum_bin input[] = {{10, {.001, 0.}}, {11, {.001, 0.}}};
        rfmodel_amplifier_component output[32];
        rfmodel_amplifier_drive drive;
        size_t written;
        if (rfmodel_multitone_amplifier_evaluate(
                1e8, input, 2, 20., 20., 23., 20., 10., 50., output, 32, &written, &drive) !=
                RFMODEL_OK ||
            written != 14 || output[6].order != 3 || output[6].index != 9 ||
            fabs(output[6].amplitude.real / -1e-6 - 1.) > 1e-12) {
            return 20;
        }
    }
    {
        const rfmodel_spectrum_bin input = {10, {.001, 0.}};
        rfmodel_spectrum_bin output[3];
        size_t written;
        if (rfmodel_single_tone_amplifier_transmit(
                1e6, &input, 1, 20., 20., 23., 20., 10., 50., output, 3, &written) != RFMODEL_OK ||
            written != 3 || output[1].index != 20 ||
            fabs(output[1].amplitude.real * output[1].amplitude.real / 2.5e-10 - 1.) > 1e-12) {
            return 19;
        }
    }
    {
        const rfmodel_complex input = {0., .1};
        rfmodel_complex output;
        if (rfmodel_saturating_fundamental(20., 20., 23., input, .01, &output) != RFMODEL_OK ||
            fabs(output.imag * output.imag / .19922937036162172 - 1.) > 1e-12) {
            return 17;
        }
    }
    {
        const rfmodel_spectrum_bin input = {10, {.001, 0.}};
        rfmodel_spectrum_bin output[4];
        size_t written;
        if (rfmodel_intercept_amplifier_transmit(
                1e6, &input, 1, 20., 20., 10., 50., output, 4, &written) != RFMODEL_OK ||
            written != 4 || output[2].index != 20 ||
            fabs(output[2].amplitude.real / sqrt(2.5e-10) - 1.) > 1e-12) {
            return 16;
        }
    }
    {
        const double coefficients[] = {.5};
        rfmodel_spectrum_bin output;
        size_t written;
        if (rfmodel_polynomial_amplifier_transmit(
                1e6, NULL, 0, coefficients, 1, 50., &output, 1, &written) != RFMODEL_OK ||
            written != 1 || output.index != 0 ||
            fabs(output.amplitude.real - .5 / sqrt(50.)) > 1e-12) {
            return 15;
        }
    }
    {
        const rfmodel_spectrum_bin bins[] = {{10, {.001, 0.}}, {20, {.002, 0.}}};
        const rfmodel_incident_spectrum port = {1e6, bins, 2};
        rfmodel_complex output;
        double total;
        if (rfmodel_p1db_spectral_fundamental(20., 10., &port, 1, 0, 10, &output, &total) !=
                RFMODEL_OK ||
            fabs(total / 5e-6 - 1.) > 1e-12 || output.real <= 0. || output.real >= .01) {
            return 14;
        }
    }
    {
        const rfmodel_complex input = {0., sqrt(pow(10., -3.9))};
        rfmodel_complex output;
        if (rfmodel_p1db_fundamental(20., 10., input, &output) != RFMODEL_OK ||
            fabs(output.imag - .1) > 1e-12) {
            return 12;
        }
        {
            const rfmodel_complex partial = {0., input.imag / 2.};
            if (rfmodel_p1db_driven_fundamental(20., 10., partial, pow(10., -3.9), &output) !=
                    RFMODEL_OK ||
                fabs(output.imag - .05) > 1e-12) {
                return 13;
            }
        }
    }
    {
        rfmodel_touchstone *model = NULL;
        rfmodel_complex reflection;
        if (argc != 2 || rfmodel_touchstone_open(argv[1], 0, &model) != RFMODEL_OK) {
            return 9;
        }
        status = rfmodel_touchstone_s(model, 1e6, 50., &reflection, 1);
        {
            rfmodel_complex absent_noise = {123., 0.};
            if (rfmodel_touchstone_noise(model, 1e6, 50., 290., &absent_noise, 1) !=
                    RFMODEL_INVALID_ARGUMENT ||
                absent_noise.real != 123.) {
                rfmodel_touchstone_close(model);
                return 11;
            }
        }
        rfmodel_touchstone_close(model);
        if (status != RFMODEL_OK || fabs(reflection.real - 0.2) > 1e-12) {
            return 10;
        }
    }
    {
        const rfmodel_complex zero_noise = {0., 0.}, intrinsic = {1., 0.}, gamma = {0.5, 0.};
        rfmodel_complex incident_noise, outgoing_noise;
        double net;
        if (rfmodel_loaded_noise(1,
                                 &zero_noise,
                                 &intrinsic,
                                 &zero_noise,
                                 1,
                                 &gamma,
                                 1,
                                 &incident_noise,
                                 &outgoing_noise,
                                 1,
                                 &net,
                                 1) != RFMODEL_OK ||
            fabs(net + 0.75) > 1e-12) {
            return 8;
        }
    }
    {
        const rfmodel_spectrum_bin input = {10, {1., 0.}};
        rfmodel_spectrum_bin output[2];
        size_t produced;
        if (rfmodel_ideal_mixer_transmit(1e6, &input, 1, 2, 0., 0., 50., output, 2, &produced) !=
                RFMODEL_OK ||
            produced != 2 || output[0].index != 8 || fabs(output[0].amplitude.real - 1.) > 1e-12) {
            return 7;
        }
    }
    {
        const rfmodel_complex matched = {50., 0.};
        if (rfmodel_linear_amplifier_s(1e9, 20., 90., 50., 0., matched, matched, 50., result, 4) !=
                RFMODEL_OK ||
            fabs(result[2].imag - 10.) > 1e-12) {
            return 6;
        }
    }
    if (rfmodel_rlgc_line_s(0., 25., 0., 0., 0., 2., 50., result, 4) != RFMODEL_OK ||
        fabs(result[2].real - 2. / 3.) > 1e-12) {
        return 4;
    }
    if (rfmodel_transmission_line_s(1e9, 100., 0.25e-9, 0., 50., result, 4) != RFMODEL_OK ||
        fabs(result[0].real - 0.6) > 1e-12) {
        return 5;
    }
    if (rfmodel_abi_version() != 1 || rfmodel_network_create(50., &network) != RFMODEL_OK) {
        return 1;
    }
    status = rfmodel_network_add(network, 2, s, 4, 50., &offset);
    if (status == RFMODEL_OK) {
        status = rfmodel_network_external_s(network, ports, 2, result, 4);
    }
    if (status == RFMODEL_OK) {
        rfmodel_complex covariance[4], output_noise[4];
        status = rfmodel_passive_noise(2, s, 4, 290., covariance, 4);
        if (status == RFMODEL_OK) {
            status =
                rfmodel_network_external_noise(network, ports, 2, covariance, 4, output_noise, 4);
            if (status == RFMODEL_OK &&
                fabs(output_noise[0].real / (1.380649e-23 * 290.) - 0.75) > 1e-12) {
                status = RFMODEL_INTERNAL_ERROR;
            }
        }
    }
    if (status == RFMODEL_OK) {
        const rfmodel_spectrum_bin input = {1, {2., 0.}};
        rfmodel_spectrum_bin output;
        size_t produced = 0;
        status = rfmodel_network_transmit_spectrum(
            network, ports, 2, 1e6, &input, 1, &output, 1, &produced);
        if (status == RFMODEL_OK &&
            (produced != 1 || output.index != 1 || fabs(output.amplitude.imag + 1.) > 1e-12)) {
            status = RFMODEL_INTERNAL_ERROR;
        }
    }
    rfmodel_network_destroy(network);
    if (status != RFMODEL_OK) {
        return 2;
    }
    return fabs(result[2].imag + 0.5) < 1e-12 && fabs(result[2].real) < 1e-12 ? 0 : 3;
}
