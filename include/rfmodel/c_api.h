#ifndef RFMODEL_C_API_H
#define RFMODEL_C_API_H
#include <stddef.h>

#if defined(_WIN32)
#if defined(RFMODEL_C_API_BUILD)
#define RFMODEL_API __declspec(dllexport)
#else
#define RFMODEL_API __declspec(dllimport)
#endif
#else
#define RFMODEL_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rfmodel_network rfmodel_network;
typedef struct rfmodel_touchstone rfmodel_touchstone;

typedef struct rfmodel_touchstone_info {
    size_t ports;
    double reference_ohms;
    double minimum_frequency_hz;
    double maximum_frequency_hz;
    size_t noise_sample_count;
} rfmodel_touchstone_info;

typedef struct rfmodel_complex {
    double real;
    double imag;
} rfmodel_complex;

/* Single-tone fundamental wave in sqrt(W), matched ports, no AM/PM.
 * Rejects power above the P1dB domain; does not predict harmonics or saturation.
 * output must be non-NULL and remains unchanged on failure. */
RFMODEL_API int rfmodel_p1db_fundamental(double power_gain_db,
                                         double output_p1db_dbm,
                                         rfmodel_complex incident,
                                         rfmodel_complex *output);

/* Fundamental-only response driven by total incident RF power (W).
 * Total power must include |incident|^2 and remain at or below input P1dB.
 * Caller is responsible for solving other frequencies/ports. No harmonic generation.
 * output must be non-NULL and remains unchanged on failure. */
RFMODEL_API int rfmodel_p1db_driven_fundamental(double power_gain_db,
                                                double output_p1db_dbm,
                                                rfmodel_complex incident,
                                                double total_incident_power_w,
                                                rfmodel_complex *output);

/* Snapshot a legacy .sNp file at a UTF-8 path. out_of_band: 0 reject, 1 clamp.
 * A failed open sets *out to NULL. close(NULL) is valid.
 * info reports embedded noise presence; s does not implicitly evaluate noise.
 * s interpolates real/imaginary components at the original reference, then
 * renormalizes to the requested positive real reference. Row-major ports^2
 * output values are written only on success. */
RFMODEL_API int
rfmodel_touchstone_open(const char *path_utf8, int out_of_band, rfmodel_touchstone **out);
RFMODEL_API void rfmodel_touchstone_close(rfmodel_touchstone *model);
RFMODEL_API int rfmodel_touchstone_get_info(const rfmodel_touchstone *model,
                                            rfmodel_touchstone_info *info);
RFMODEL_API int rfmodel_touchstone_s(const rfmodel_touchstone *model,
                                     double frequency_hz,
                                     double reference_ohms,
                                     rfmodel_complex *values,
                                     size_t capacity);

enum rfmodel_status {
    RFMODEL_OK = 0,
    RFMODEL_INVALID_ARGUMENT = 1,
    RFMODEL_SOLVER_ERROR = 2,
    RFMODEL_OUT_OF_MEMORY = 3,
    RFMODEL_INTERNAL_ERROR = 4
};

/* Evaluate embedded noise covariance in W/Hz at the requested wave reference.
 * reference_temperature_k is the positive NF reference temperature, not physical
 * device temperature. Missing/invalid noise data is an error, never zero noise.
 * Uses the snapshot and range policy selected at open; failure leaves values unchanged. */
RFMODEL_API int rfmodel_touchstone_noise(const rfmodel_touchstone *model,
                                         double frequency_hz,
                                         double reference_ohms,
                                         double reference_temperature_k,
                                         rfmodel_complex *values,
                                         size_t capacity);

typedef struct rfmodel_spectrum_bin {
    int index;
    rfmodel_complex amplitude;
} rfmodel_spectrum_bin;

/* Transmit through a fixed-S network, with matched external source/load.
 * external_ports has exactly two entries, ordered input then output.
 * Internal reflections are solved. Frequency-dependent models must be rebuilt
 * per frequency. Output follows the spectrum buffer contract below. */
RFMODEL_API int rfmodel_network_transmit_spectrum(const rfmodel_network *network,
                                                  const size_t *external_ports,
                                                  size_t external_count,
                                                  double spacing_hz,
                                                  const rfmodel_spectrum_bin *input,
                                                  size_t input_count,
                                                  rfmodel_spectrum_bin *output,
                                                  size_t capacity,
                                                  size_t *output_count);

/* Fixed sufficient output allocation for the current sparse-spectrum kernels. */
#define RFMODEL_SPECTRUM_CAPACITY 4096

/* Nonnegative integer bins, frequency=index*spacing_hz; amplitudes in sqrt(W).
   Input bins must be unique; DC must be real. No output/count changes on error. */
RFMODEL_API int rfmodel_cubic_amplifier_transmit(double spacing_hz,
                                                 const rfmodel_spectrum_bin *input,
                                                 size_t input_count,
                                                 double power_gain_db,
                                                 double input_ip3_dbm,
                                                 double reference_ohms,
                                                 rfmodel_spectrum_bin *output,
                                                 size_t capacity,
                                                 size_t *output_count);
RFMODEL_API int rfmodel_ideal_mixer_transmit(double spacing_hz,
                                             const rfmodel_spectrum_bin *input,
                                             size_t input_count,
                                             int lo_bin,
                                             double conversion_gain_db,
                                             double lo_phase_radians,
                                             double reference_ohms,
                                             rfmodel_spectrum_bin *output,
                                             size_t capacity,
                                             size_t *output_count);

/* Error text belongs to this thread; valid until its next status-returning call. */
RFMODEL_API const char *rfmodel_last_error(void);
RFMODEL_API unsigned int rfmodel_abi_version(void);
RFMODEL_API int rfmodel_network_create(double reference_ohms, rfmodel_network **out);
RFMODEL_API void rfmodel_network_destroy(rfmodel_network *network);
/* Row-major complex S matrix; value_count must equal ports*ports. Input is copied. */
RFMODEL_API int rfmodel_network_add(rfmodel_network *network,
                                    size_t ports,
                                    const rfmodel_complex *values,
                                    size_t value_count,
                                    double reference_ohms,
                                    size_t *first_port);
RFMODEL_API int rfmodel_network_connect(rfmodel_network *network, size_t first, size_t second);
RFMODEL_API int rfmodel_network_terminate(rfmodel_network *network,
                                          size_t port,
                                          rfmodel_complex reflection,
                                          rfmodel_complex source);
RFMODEL_API int rfmodel_network_port_count(const rfmodel_network *network, size_t *count);
/* Caller-owned, non-overlapping arrays; capacity is in complex elements. */
RFMODEL_API int rfmodel_network_solve(const rfmodel_network *network,
                                      rfmodel_complex *incident,
                                      rfmodel_complex *outgoing,
                                      size_t capacity,
                                      double *relative_residual);
/* Selected ports remain unassigned; other ports must be connected/terminated. */
RFMODEL_API int rfmodel_network_external_s(const rfmodel_network *network,
                                           const size_t *ports,
                                           size_t port_count,
                                           rfmodel_complex *values,
                                           size_t capacity);

/* Two-port parameter models, four row-major output elements. All units are SI
   except propagation_loss_db. Reference impedance is real and positive. */
RFMODEL_API int rfmodel_transmission_line_s(double frequency_hz,
                                            double characteristic_ohms,
                                            double delay_s,
                                            double propagation_loss_db,
                                            double reference_ohms,
                                            rfmodel_complex *values,
                                            size_t capacity);
RFMODEL_API int rfmodel_rlgc_line_s(double frequency_hz,
                                    double resistance_ohms_per_m,
                                    double inductance_h_per_m,
                                    double conductance_s_per_m,
                                    double capacitance_f_per_m,
                                    double length_m,
                                    double reference_ohms,
                                    rfmodel_complex *values,
                                    size_t capacity);

/* Bilateral small-signal two-port; no compression, automatic noise or DC blocking. */
RFMODEL_API int rfmodel_linear_amplifier_s(double frequency_hz,
                                           double gain_db,
                                           double gain_phase_degrees,
                                           double reverse_isolation_db,
                                           double reverse_phase_degrees,
                                           rfmodel_complex input_impedance_ohms,
                                           rfmodel_complex output_impedance_ohms,
                                           double reference_ohms,
                                           rfmodel_complex *values,
                                           size_t capacity);

/* Covariance matrices use W/Hz. Outputs are written only on success. */
RFMODEL_API int rfmodel_passive_noise(size_t ports,
                                      const rfmodel_complex *scattering,
                                      size_t value_count,
                                      double temperature_k,
                                      rfmodel_complex *covariance,
                                      size_t capacity);
/* Intrinsic covariance covers ALL global ports, including cross-device correlations.
   Selected external ports are matched/noiseless. Termination noise is not automatic. */
RFMODEL_API int rfmodel_network_external_noise(const rfmodel_network *network,
                                               const size_t *ports,
                                               size_t port_count,
                                               const rfmodel_complex *intrinsic,
                                               size_t value_count,
                                               rfmodel_complex *covariance,
                                               size_t capacity);

/* All matrices have value_count=ports*ports, reflections has reflection_count=ports.
   Output arrays must not overlap each other; nothing is written on failure. */
RFMODEL_API int rfmodel_loaded_noise(size_t ports,
                                     const rfmodel_complex *scattering,
                                     const rfmodel_complex *intrinsic,
                                     const rfmodel_complex *boundary_emission,
                                     size_t value_count,
                                     const rfmodel_complex *reflections,
                                     size_t reflection_count,
                                     rfmodel_complex *incident,
                                     rfmodel_complex *outgoing,
                                     size_t matrix_capacity,
                                     double *net_into_device,
                                     size_t power_capacity);
RFMODEL_API int rfmodel_thermal_boundary_noise(size_t ports,
                                               const rfmodel_complex *reflections,
                                               const double *temperatures_k,
                                               rfmodel_complex *covariance,
                                               size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
