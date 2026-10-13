#include "rfmodel/api/c_api.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, rfmodel_last_error());                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static rfmodel_complex add(rfmodel_complex a, rfmodel_complex b) {
    rfmodel_complex r = {a.real + b.real, a.imag + b.imag};
    return r;
}

static rfmodel_complex mul(rfmodel_complex a, rfmodel_complex b) {
    rfmodel_complex r = {a.real * b.real - a.imag * b.imag, a.real * b.imag + a.imag * b.real};
    return r;
}

static rfmodel_complex cj(rfmodel_complex a) {
    a.imag = -a.imag;
    return a;
}

static int close_to(rfmodel_complex a, rfmodel_complex b) {
    return hypot(a.real - b.real, a.imag - b.imag) < 1e-12;
}

int main(void) {
    const size_t ports[1] = {0};
    int bins[1] = {1};
    const rfmodel_complex a[1] = {{.3, .1}}, b[1] = {{.1, -.02}};
    const rfmodel_complex source[1] = {{.4, -.2}}, offset[1] = {{-.3, .5}};
    const rfmodel_complex sc[1] = {{2, 0}}, sp[1] = {{.4, .2}};
    const rfmodel_complex ic[1] = {{.3, 0}}, ip[1] = {{.1, 0}};
    const rfmodel_complex ec[1] = {{1, 0}}, ep[1] = {{.2, -.1}};
    rfmodel_complex z[1] = {{31, 12}};
    rfmodel_conversion_request request = {
        1, 1, NAN, ports, bins, a, b, source, NULL, sc, sp, ic, ip};
    rfmodel_conversion_references refs = {1, z};
    const rfmodel_conversion_affine_offset emission = {1, offset};
    const rfmodel_conversion_source_noise additional = {1, ec, ep};
    rfmodel_complex incident[2], outgoing[2], c[2], p[2];
    rfmodel_complex caa[2], paa[2], cab[2], pab[2];
    double power[2], residual;
    rfmodel_conversion_output output = {incident, outgoing, c, p, 2, 2, &residual};
    rfmodel_conversion_loaded_output loaded = {caa, paa, cab, pab, power, 2, 2};
    const rfmodel_complex sentinel = {91, -32};
    rfmodel_complex expected, total_c = {3, 0}, total_p = {.6, .1};
    incident[1] = outgoing[1] = c[1] = p[1] = sentinel;
    caa[1] = paa[1] = cab[1] = pab[1] = sentinel;
    power[1] = 99;
    CHECK(rfmodel_conversion_analyze_power_waves(
              &request, &refs, &emission, &additional, &output, &loaded) == RFMODEL_OK);
    expected = add(add(mul(a[0], source[0]), mul(b[0], cj(source[0]))), offset[0]);
    CHECK(close_to(incident[0], source[0]) && close_to(outgoing[0], expected));
    expected = add(add(mul(add(mul(a[0], cj(a[0])), mul(b[0], cj(b[0]))), total_c),
                       mul(mul(a[0], cj(b[0])), total_p)),
                   add(mul(mul(b[0], cj(a[0])), cj(total_p)), ic[0]));
    CHECK(close_to(c[0], expected));
    expected = add(add(mul(mul(a[0], a[0]), total_p), mul(mul(b[0], b[0]), cj(total_p))),
                   add(mul(mul(a[0], b[0]), (rfmodel_complex){6, 0}), ip[0]));
    CHECK(close_to(p[0], expected));
    CHECK(close_to(caa[0], total_c) && close_to(paa[0], total_p));
    CHECK(close_to(cab[0], add(mul(total_c, cj(a[0])), mul(total_p, cj(b[0])))));
    CHECK(close_to(pab[0], add(mul(total_p, a[0]), mul(total_c, b[0]))));
    CHECK(fabs(power[0] - (3 - c[0].real)) < 1e-12 && residual < 1e-12);
    CHECK(close_to(incident[1], sentinel) && close_to(outgoing[1], sentinel) &&
          close_to(c[1], sentinel) && close_to(p[1], sentinel) && close_to(caa[1], sentinel) &&
          close_to(paa[1], sentinel) && close_to(cab[1], sentinel) && close_to(pab[1], sentinel) &&
          power[1] == 99);
    CHECK(rfmodel_conversion_analyze(&request, &output) != RFMODEL_OK);
    /* Inputs, descriptors, full capacity ranges and all outputs remain unchanged. */
    for (int scenario = 0; scenario < 15; ++scenario) {
        rfmodel_conversion_request r = request;
        rfmodel_conversion_references ref = refs;
        rfmodel_conversion_output o = output;
        const rfmodel_conversion_references *rp = &ref;
        rfmodel_complex local_z = z[0];
        rfmodel_complex save_incident[2], save_outgoing[2], save_c[2], save_p[2];
        double saved_residual = residual;
        ref.values = &local_z;
        switch (scenario) {
        case 0:
            rp = NULL;
            break;
        case 1:
            ref.count = 0;
            break;
        case 2:
            ref.count = SIZE_MAX;
            break;
        case 3:
            ref.values = NULL;
            break;
        case 4:
            local_z.real = 0;
            break;
        case 5:
            local_z.real = -1;
            break;
        case 6:
            local_z.real = INFINITY;
            break;
        case 7:
            local_z.imag = NAN;
            break;
        case 8:
            bins[0] = 0;
            break;
        case 9:
            o.incident = &local_z;
            break;
        case 10:
            o.incident = (rfmodel_complex *)&ref;
            break;
        case 11:
            o.incident = (rfmodel_complex *)&r;
            break;
        case 12:
            o.matrix_capacity = SIZE_MAX;
            break;
        case 13:
            o.wave_capacity = 0;
            break;
        case 14:
            o.noise_covariance = o.outgoing;
            break;
        }
        memcpy(save_incident, incident, sizeof(incident));
        memcpy(save_outgoing, outgoing, sizeof(outgoing));
        memcpy(save_c, c, sizeof(c));
        memcpy(save_p, p, sizeof(p));
        {
            rfmodel_complex saved_z = local_z;
            rfmodel_conversion_references saved_ref = ref;
            rfmodel_conversion_request saved_request = r;
            CHECK(rfmodel_conversion_analyze_power_waves(&r, rp, NULL, NULL, &o, NULL) !=
                  RFMODEL_OK);
            CHECK(memcmp(&saved_z, &local_z, sizeof(local_z)) == 0);
            CHECK(memcmp(&saved_ref, &ref, sizeof(ref)) == 0);
            CHECK(memcmp(&saved_request, &r, sizeof(r)) == 0);
        }
        CHECK(memcmp(save_incident, incident, sizeof(incident)) == 0 &&
              memcmp(save_outgoing, outgoing, sizeof(outgoing)) == 0 &&
              memcmp(save_c, c, sizeof(c)) == 0 && memcmp(save_p, p, sizeof(p)) == 0 &&
              saved_residual == residual);
        bins[0] = 1;
    }
    /* Two independently matched devices, with affine emission on an internal wire. */
    {
        const rfmodel_complex zero[1] = {{0, 0}}, offsets[2] = {{1, 0}, {0, 0}};
        const rfmodel_complex z1[1] = {{25, 10}}, z2[1] = {{100, -20}};
        rfmodel_conversion_request devices[2] = {
            {1, 1, NAN, ports, bins, zero, zero, NULL, NULL, NULL, NULL, NULL, NULL},
            {1, 1, NAN, ports, bins, zero, zero, NULL, NULL, NULL, NULL, NULL, NULL}};
        const rfmodel_conversion_references references[2] = {{1, z1}, {1, z2}};
        const rfmodel_conversion_connection wire = {0, 0, 1, 0};
        const rfmodel_conversion_affine_offset affine = {2, offsets};
        rfmodel_complex incident2[2], outgoing2[2], covariance[4], complementary[4];
        rfmodel_conversion_output out = {
            incident2, outgoing2, covariance, complementary, 2, 4, &residual};
        const double denominator = 125. * 125. + 10. * 10.;
        const rfmodel_complex first = {(75. * 125. + 10. * 10.) / denominator,
                                       (10. * 125. - 75. * 10.) / denominator};
        const rfmodel_complex second = {100. * 125. / denominator, -100. * 10. / denominator};
        CHECK(rfmodel_conversion_network_analyze_power_waves(
                  devices, 2, &wire, 1, references, &affine, NULL, &out, NULL) == RFMODEL_OK);
        CHECK(close_to(outgoing2[0], offsets[0]) && close_to(outgoing2[1], offsets[1]));
        CHECK(close_to(incident2[0], first) && close_to(incident2[1], second));
        CHECK(residual < 1e-12);
    }
    return 0;
}
