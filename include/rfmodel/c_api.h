#ifndef RFMODEL_C_API_H
#define RFMODEL_C_API_H
#include <stddef.h>
#include <stdint.h>

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

enum rfmodel_intercept_reference {
    RFMODEL_INTERCEPT_INPUT = 0,
    RFMODEL_INTERCEPT_OUTPUT = 1
};

typedef struct rfmodel_two_tone_intercept {
    int first_tone_order;
    int second_tone_order;
    double intercept_dbm;
    int coefficient_sign;
    int reference;
} rfmodel_two_tone_intercept;

/* Extrapolated equal per-tone intercepts for k1*f1+k2*f2. Both k values
 * are nonzero; abs(k1)+abs(k2) is 2..11 and unique per entry. Sign is +/-1.
 * Input/output reference affects IP units, not the required positive real R.
 * At most ten entries; omitted nonlinear orders are zero. Empty entries
 * produce [0, amplitude_gain]. Returns coefficients a[0]..a[maximum_order].
 * This is not a P1dB fit or an automatic RFAMP higher-order coefficient rule.
 * Output and count are required, capacity must suffice (up to 12). All
 * buffers/scalars must be non-overlapping; all outputs are unchanged on failure. */
RFMODEL_API int
rfmodel_polynomial_coefficients_from_intercepts(double power_gain_db,
                                                const rfmodel_two_tone_intercept *intercepts,
                                                size_t intercept_count,
                                                double reference_ohms,
                                                double *coefficients,
                                                size_t capacity,
                                                size_t *coefficient_count);

/* Output powers IM1..IMn in dBm, n=1..11, using the RFAMP_HO reference
 * products documented by SystemVue 2023. Exactly n-1 signs (+/-1) are
 * required for a2..an; powers cannot determine signs. IM1 alone is linear.
 * Returns a0..an, not a saturation fit. All input/output arrays and the
 * output count must be disjoint; every output stays unchanged on failure. */
RFMODEL_API int
rfmodel_polynomial_coefficients_from_intermod_levels(double power_gain_db,
                                                     const double *output_levels_dbm,
                                                     size_t level_count,
                                                     const int *coefficient_signs,
                                                     size_t sign_count,
                                                     double reference_ohms,
                                                     double *coefficients,
                                                     size_t capacity,
                                                     size_t *coefficient_count);

/* Per-port complex Kurokawa power-wave references, Re(Z)>0.
 * Row-major ports^2 scattering/noise matrices; each reference array has ports
 * entries. Noise input and output must be both NULL or both non-NULL.
 * Outputs must not overlap inputs or one another; read-only inputs may alias.
 * Outputs remain unchanged on every failure. */
RFMODEL_API int rfmodel_power_wave_renormalize(size_t ports,
                                               const rfmodel_complex *scattering,
                                               size_t value_count,
                                               const rfmodel_complex *old_references,
                                               const rfmodel_complex *new_references,
                                               const rfmodel_complex *intrinsic_noise,
                                               rfmodel_complex *new_scattering,
                                               rfmodel_complex *new_noise,
                                               size_t capacity);

/* admittance=0 selects physical Z (ohm); admittance=1 selects Y (siemens).
 * References use the same power-wave convention. Singular conversions fail.
 * Output must not overlap inputs and remains unchanged on failure. */
RFMODEL_API int rfmodel_power_wave_s_to_parameters(size_t ports,
                                                   const rfmodel_complex *scattering,
                                                   size_t value_count,
                                                   const rfmodel_complex *references,
                                                   int admittance,
                                                   rfmodel_complex *output,
                                                   size_t capacity);

typedef struct rfmodel_source_coherence {
    const char *source_id;
    const char *reference_clock;
} rfmodel_source_coherence;

/* Up to 4096 unique sources. Labels are NUL-terminated, case-sensitive bytes,
 * at most 1024 bytes; source_id is nonempty. NULL/empty clock means independent.
 * Same nonempty clock gets the same group; independent source names cannot
 * collide with clock names. Output follows input order and is permutation-stable
 * for a fixed source set. IDs may change when the source set changes.
 * Empty input accepts NULL arrays. Output has exactly count elements; all buffers
 * must be non-overlapping and groups remain unchanged on any failure.
 * This resolves source clocks only, not harmonic/intermod/LO relationships. */
RFMODEL_API int rfmodel_assign_source_coherence(const rfmodel_source_coherence *sources,
                                                size_t count,
                                                uint64_t *groups,
                                                size_t capacity);

enum rfmodel_spectrum_kind {
    RFMODEL_SPECTRUM_SOURCE = 0,
    RFMODEL_SPECTRUM_HARMONIC = 1,
    RFMODEL_SPECTRUM_INTERMOD = 2
};

typedef struct rfmodel_coherent_component {
    int index;
    int kind;
    double bandwidth_hz;
    uint64_t coherence_group;
    rfmodel_complex amplitude;
} rfmodel_coherent_component;

typedef struct rfmodel_coherent_mixer_input {
    rfmodel_coherent_component component;
    int lo_index;
    double conversion_gain_db;
    double lo_phase_radians;
    uint64_t lo_coherence_group;
} rfmodel_coherent_mixer_input;

/* Prescribed noiseless CW LOs, equal positive real RF/IF reference resistances.
 * Batch all parallel branches (<=2048). Output is exactly 2*input_count:
 * [difference, sum] per input, including zero waves. Kind/bandwidth are retained.
 * Equal (RF group, LO group) pairs share new IDs above every input/LO ID and
 * reserved_group_max. Reserve bypass group IDs there before later combination.
 * IDs are local to this batch; never combine separately assigned batch outputs.
 * RF/LO equality (DC), bands crossing DC, invalid/overflowed values are errors.
 * NULL arrays are valid for empty input; output_count is always required.
 * Buffers/scalars must not overlap. All outputs remain unchanged on failure. */
RFMODEL_API int rfmodel_mix_coherent_components(double spacing_hz,
                                                const rfmodel_coherent_mixer_input *input,
                                                size_t input_count,
                                                uint64_t reserved_group_max,
                                                rfmodel_coherent_component *output,
                                                size_t output_capacity,
                                                size_t *output_count);

typedef struct rfmodel_bin_power {
    int index;
    double power_w;
} rfmodel_bin_power;

/* Up to 4096 deterministic RF components, RMS power waves in sqrt(W).
 * Equal (index,kind,bandwidth,group) keys add amplitudes; other groups add power.
 * Positive nonzero group IDs must be assigned by the caller; no clock inference.
 * Zero-amplitude merged groups remain. Group and bin-power arrays are sorted.
 * All arrays/scalars must be non-overlapping. On any failure all outputs remain
 * unchanged. Empty input accepts NULL arrays but requires both counts and total.
 * This is not a noise PSD or general overlapping-band integration operation. */
RFMODEL_API int rfmodel_reduce_coherent_components(double spacing_hz,
                                                   const rfmodel_coherent_component *input,
                                                   size_t input_count,
                                                   rfmodel_coherent_component *groups,
                                                   size_t group_capacity,
                                                   size_t *group_count,
                                                   rfmodel_bin_power *powers,
                                                   size_t power_capacity,
                                                   size_t *power_count,
                                                   double *total_power_w);

/* Shared cubic/tanh fundamental compression after coherent input reduction.
 * Source-kind carriers only. Preserves group/phase/bandwidth; no new distortion.
 * Matched forward input; no noise or reverse-wave/feedback solution.
 * Output buffer and atomic-failure rules match reduce_coherent_components.
 * input_power_w is also required and remains unchanged on any failure. */
RFMODEL_API int rfmodel_compress_coherent_fundamentals(double spacing_hz,
                                                       double power_gain_db,
                                                       double output_p1db_dbm,
                                                       double output_saturation_dbm,
                                                       const rfmodel_coherent_component *input,
                                                       size_t input_count,
                                                       rfmodel_coherent_component *groups,
                                                       size_t group_capacity,
                                                       size_t *group_count,
                                                       rfmodel_bin_power *powers,
                                                       size_t power_capacity,
                                                       size_t *power_count,
                                                       double *output_power_w,
                                                       double *input_power_w);

typedef struct rfmodel_port_coherent_component {
    size_t input_port;
    rfmodel_coherent_component component;
} rfmodel_port_coherent_component;

/* Propagate matched incident components through a fixed-S connected network.
 * Ports are GLOBAL indices included in external_ports; output is an outgoing wave.
 * Internal feedback is solved; no implicit noise or external source reflection.
 * Buffer, empty-input and atomic-failure rules match reduce_coherent_components. */
RFMODEL_API int rfmodel_network_transmit_coherent(const rfmodel_network *network,
                                                  const size_t *external_ports,
                                                  size_t port_count,
                                                  size_t output_port,
                                                  double spacing_hz,
                                                  const rfmodel_port_coherent_component *input,
                                                  size_t input_count,
                                                  rfmodel_coherent_component *groups,
                                                  size_t group_capacity,
                                                  size_t *group_count,
                                                  rfmodel_bin_power *powers,
                                                  size_t power_capacity,
                                                  size_t *power_count,
                                                  double *total_power_w);

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

/* Cubic below P1dB, incremental tanh above, phase-preserving fundamental only.
 * OPSAT must exceed OP1dB. Total finite power includes |incident|^2.
 * No harmonic generation, AM/PM or nonlinear feedback solution.
 * output must be non-NULL and remains unchanged on failure. */
RFMODEL_API int rfmodel_saturating_fundamental(double power_gain_db,
                                               double output_p1db_dbm,
                                               double output_saturation_dbm,
                                               rfmodel_complex incident,
                                               double total_incident_power_w,
                                               rfmodel_complex *output);

/* Common amplitude gain at a finite nonnegative physical total drive, including
 * zero. Use only after coherent summation; source contributions are not drive.
 * No AM/PM or noise. output is required and unchanged on failure. */
RFMODEL_API int rfmodel_saturating_amplitude_gain(double total_incident_power_w,
                                                  double power_gain_db,
                                                  double output_p1db_dbm,
                                                  double output_saturation_dbm,
                                                  double *output);

typedef struct rfmodel_amplifier_operating_point {
    double fundamental_amplitude_gain;
    double nonlinear_input_scale;
    double limited_input_power_w;
    double quadratic_voltage_coefficient;
    double cubic_voltage_coefficient;
} rfmodel_amplifier_operating_point;

/* Shared RFAMP approximation response at externally solved physical drive.
 * Nonlinear coefficients are RAW voltage coefficients: scale carrier waves by
 * nonlinear_input_scale first. Existing distortion receives only fundamental gain.
 * At zero drive scale=1 and limited power=0. output unchanged on any failure. */
RFMODEL_API int rfmodel_get_amplifier_operating_point(double total_incident_power_w,
                                                      double power_gain_db,
                                                      double output_p1db_dbm,
                                                      double output_saturation_dbm,
                                                      double input_ip2_dbm,
                                                      double input_ip3_dbm,
                                                      double reference_ohms,
                                                      rfmodel_amplifier_operating_point *output);

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

/* order: 1 calibrated direct response, 2 quadratic, 3 cubic products.
 * Different orders at the same bin stay separate; no implicit coherent sum. */
typedef struct rfmodel_amplifier_component {
    int order;
    int index;
    rfmodel_complex amplitude;
} rfmodel_amplifier_component;

typedef struct rfmodel_amplifier_drive {
    double total_input_power_w;
    double limited_input_power_w;
} rfmodel_amplifier_drive;

typedef struct rfmodel_origin_factor {
    uint64_t root_id;
    int sign; /* +1 or -1 (conjugate) */
} rfmodel_origin_factor;

typedef struct rfmodel_mixing_origin {
    const rfmodel_origin_factor *factors;
    size_t count;
} rfmodel_mixing_origin;

/* Compose 1..11 signed one-based parent indices; retain repeated/opposite factors.
 * At most 4096 parents, 256 factors per parent/output and 65536 parent factors.
 * Canonical output is sorted by (root_id, sign). Root IDs must be nonzero.
 * Output and count must not overlap; both stay unchanged on any failure. */
RFMODEL_API int rfmodel_expand_mixing_origin(const rfmodel_mixing_origin *parents,
                                             size_t parent_count,
                                             const int *indices,
                                             size_t index_count,
                                             rfmodel_origin_factor *output,
                                             size_t capacity,
                                             size_t *count);

/* Contributions belong to ONE coherent component. The amplitude already includes
 * source waves and path gains; root factors describe provenance, not values to
 * multiply into amplitude a second time. Empty expressions represent zero. */
typedef struct rfmodel_origin_contribution {
    const rfmodel_origin_factor *factors;
    size_t factor_count;
    rfmodel_complex amplitude;
} rfmodel_origin_contribution;

typedef struct rfmodel_origin_expression {
    const rfmodel_origin_contribution *terms;
    size_t term_count;
} rfmodel_origin_expression;

typedef struct rfmodel_origin_expression_term {
    size_t factor_offset;
    size_t factor_count;
    rfmodel_complex amplitude;
} rfmodel_origin_expression_term;

/* Canonical terms and flattened factors. Max 4096 output terms/65536 factors.
 * All output buffers and counters must be disjoint; failures change no outputs.
 * Opposite factors do not cancel; zero-amplitude terms retain their identities. */
RFMODEL_API int rfmodel_sum_origin_expressions(const rfmodel_origin_expression *parents,
                                               size_t parent_count,
                                               rfmodel_origin_expression_term *terms,
                                               size_t term_capacity,
                                               size_t *term_count,
                                               rfmodel_origin_factor *factors,
                                               size_t factor_capacity,
                                               size_t *factor_count,
                                               rfmodel_complex *total_amplitude);

RFMODEL_API int rfmodel_product_origin_expressions(const rfmodel_origin_expression *parents,
                                                   size_t parent_count,
                                                   const int *indices,
                                                   size_t index_count,
                                                   rfmodel_complex coefficient,
                                                   rfmodel_origin_expression_term *terms,
                                                   size_t term_capacity,
                                                   size_t *term_count,
                                                   rfmodel_origin_factor *factors,
                                                   size_t factor_capacity,
                                                   size_t *factor_count,
                                                   rfmodel_complex *total_amplitude);

/* RF polynomial orders 1..9. Signed one-based reduced-input indices.
 * Harmonic/intermod inputs generate new terms. DC is projected out.
 * Generated group IDs are local; callers resolve recursive source identities. */
typedef struct rfmodel_coherent_polynomial_term {
    int order;
    int input_indices[9];
    rfmodel_coherent_component component;
} rfmodel_coherent_polynomial_term;

/* voltage_coefficients contains a[0]..a[n], 1..10 entries, with a[0]=0.
 * Up to 4096 inputs/output terms; nonlinear coefficients limit active inputs to 64.
 * Output buffers/counts must not overlap. On any failure all outputs stay unchanged. */
RFMODEL_API int rfmodel_coherent_polynomial_evaluate(double spacing_hz,
                                                     const rfmodel_coherent_component *input,
                                                     size_t input_count,
                                                     const double *voltage_coefficients,
                                                     size_t coefficient_count,
                                                     double reference_ohms,
                                                     uint64_t reserved_group_max,
                                                     rfmodel_coherent_component *reduced_inputs,
                                                     size_t reduced_capacity,
                                                     size_t *reduced_count,
                                                     rfmodel_coherent_polynomial_term *terms,
                                                     size_t term_capacity,
                                                     size_t *term_count);

/* Version 2 supports RF orders 1..11 and 1..12 voltage coefficients.
 * The original nine-index structure/function retain their layout and limit.
 * Same input, resource limits and failure contract as the original function. */
typedef struct rfmodel_coherent_polynomial_term_v2 {
    int order;
    int input_indices[11];
    rfmodel_coherent_component component;
} rfmodel_coherent_polynomial_term_v2;

RFMODEL_API int rfmodel_coherent_polynomial_evaluate_v2(double spacing_hz,
                                                        const rfmodel_coherent_component *input,
                                                        size_t input_count,
                                                        const double *voltage_coefficients,
                                                        size_t coefficient_count,
                                                        double reference_ohms,
                                                        uint64_t reserved_group_max,
                                                        rfmodel_coherent_component *reduced_inputs,
                                                        size_t reduced_capacity,
                                                        size_t *reduced_count,
                                                        rfmodel_coherent_polynomial_term_v2 *terms,
                                                        size_t term_capacity,
                                                        size_t *term_count);

/* Explicit coefficients are a2..an (0..10 entries); no constant/linear entries.
 * Empty coefficients select compressed direct propagation only. Generated terms
 * use common limited carrier inputs; existing distortion only propagates when
 * propagate_distortion=1. All output buffers/scalars must be disjoint and stay
 * unchanged on failure. Uses v2 eleven-index terms, at most 4096 combined terms. */
RFMODEL_API int
rfmodel_get_highorder_amplifier_operating_point(double total_input_power_w,
                                                double power_gain_db,
                                                double output_p1db_dbm,
                                                double output_saturation_dbm,
                                                const double *nonlinear_voltage_coefficients,
                                                size_t coefficient_count,
                                                double reference_ohms,
                                                rfmodel_amplifier_operating_point *output);

RFMODEL_API int
rfmodel_highorder_amplifier_evaluate(double spacing_hz,
                                     const rfmodel_coherent_component *input,
                                     size_t input_count,
                                     double power_gain_db,
                                     double output_p1db_dbm,
                                     double output_saturation_dbm,
                                     const double *nonlinear_voltage_coefficients,
                                     size_t coefficient_count,
                                     double reference_ohms,
                                     uint64_t reserved_group_max,
                                     int propagate_distortion,
                                     rfmodel_coherent_component *reduced_inputs,
                                     size_t reduced_capacity,
                                     size_t *reduced_count,
                                     rfmodel_coherent_polynomial_term_v2 *terms,
                                     size_t term_capacity,
                                     size_t *term_count,
                                     rfmodel_amplifier_drive *drive,
                                     rfmodel_amplifier_operating_point *operating_point);

/* Local generating input bins, ascending signed order. Negative means conjugate;
 * only the first order entries are used, with zero padding to three entries. */
typedef struct rfmodel_coherent_amplifier_term {
    int order;
    int input_indices[3];
    rfmodel_coherent_component component;
} rfmodel_coherent_amplifier_term;

/* Matched shared compression plus limited quadratic/cubic RF products.
 * Up to 4096 source-kind inputs, 64 active reduced groups, 4096 output terms.
 * reduced_inputs is sorted like reduce_coherent_components; input_indices are
 * signed ONE-BASED positions in that array, not frequency bins. Negative means
 * conjugation; unused slots are zero. Terms sort by order/bin/input_indices.
 * Direct groups persist, including zero waves. Generated bands sum contributor
 * bandwidths; unique generated IDs exceed all inputs and reserved_group_max.
 * IDs are local to this call. Compare explicit origins before combining calls.
 * No DC, recursive nonlinear inputs, noise, AM/PM or reverse feedback.
 * All buffers/scalars must not overlap. Counts and drive are always required.
 * Empty input permits NULL arrays. ALL outputs remain unchanged on failure. */
RFMODEL_API int rfmodel_coherent_amplifier_evaluate(double spacing_hz,
                                                    const rfmodel_coherent_component *input,
                                                    size_t input_count,
                                                    double power_gain_db,
                                                    double output_p1db_dbm,
                                                    double output_saturation_dbm,
                                                    double input_ip2_dbm,
                                                    double input_ip3_dbm,
                                                    double reference_ohms,
                                                    uint64_t reserved_group_max,
                                                    rfmodel_coherent_component *reduced_inputs,
                                                    size_t reduced_capacity,
                                                    size_t *reduced_count,
                                                    rfmodel_coherent_amplifier_term *terms,
                                                    size_t term_capacity,
                                                    size_t *term_count,
                                                    rfmodel_amplifier_drive *drive);

/* Cascade mode: harmonic/intermod inputs contribute to drive and propagate with
 * shared compressed gain. Only source-kind inputs generate new distortion.
 * Local order=1 denotes transmission, even for an inherited harmonic/intermod.
 * Generated origins must be resolved with conducted origins before reduction.
 * No secondary distortion mixing. Other contracts match evaluate above. */
RFMODEL_API int rfmodel_coherent_amplifier_cascade(double spacing_hz,
                                                   const rfmodel_coherent_component *input,
                                                   size_t input_count,
                                                   double power_gain_db,
                                                   double output_p1db_dbm,
                                                   double output_saturation_dbm,
                                                   double input_ip2_dbm,
                                                   double input_ip3_dbm,
                                                   double reference_ohms,
                                                   uint64_t reserved_group_max,
                                                   rfmodel_coherent_component *reduced_inputs,
                                                   size_t reduced_capacity,
                                                   size_t *reduced_count,
                                                   rfmodel_coherent_amplifier_term *terms,
                                                   size_t term_capacity,
                                                   size_t *term_count,
                                                   rfmodel_amplifier_drive *drive);

typedef struct rfmodel_amplifier_term {
    int order;
    int index;
    int contributors[3];
    rfmodel_complex amplitude;
} rfmodel_amplifier_term;

/* Propagate local mixing identities through a fixed-S network with matched
 * selected external ports. Preserve input order and zero terms. Duplicate or
 * inconsistent identities reject. Max 4096 terms / 2048 distinct frequencies.
 * Caller-owned non-overlapping arrays; output/count unchanged on failure. */
RFMODEL_API int rfmodel_network_transmit_terms(const rfmodel_network *network,
                                               const size_t *external_ports,
                                               size_t external_count,
                                               double spacing_hz,
                                               const rfmodel_amplifier_term *input,
                                               size_t input_count,
                                               rfmodel_amplifier_term *output,
                                               size_t capacity,
                                               size_t *output_count);

/* Like evaluate, but preserve individual quadratic/cubic mixing combinations.
 * Sorted by order, output bin, then contributors. At most 4096 returned terms;
 * resource exhaustion rejects the whole operation. Same atomic output contract. */
RFMODEL_API int rfmodel_multitone_amplifier_terms(double spacing_hz,
                                                  const rfmodel_spectrum_bin *input,
                                                  size_t input_count,
                                                  double power_gain_db,
                                                  double output_p1db_dbm,
                                                  double output_saturation_dbm,
                                                  double input_ip2_dbm,
                                                  double input_ip3_dbm,
                                                  double reference_ohms,
                                                  rfmodel_amplifier_term *output,
                                                  size_t capacity,
                                                  size_t *output_count,
                                                  rfmodel_amplifier_drive *drive);

/* Total-drive limited RF components; nonzero DC rejected, generated DC blocked.
 * Caller-owned non-overlapping outputs, sorted by order then bin. At most 10240
 * components. output_count and drive must be non-NULL; output can be NULL only
 * for an empty result. All outputs remain unchanged on any failure, including
 * insufficient capacity. Phase convention is positive quadratic/negative cubic;
 * this is not a validated full SystemVue multitone response. */
RFMODEL_API int rfmodel_multitone_amplifier_evaluate(double spacing_hz,
                                                     const rfmodel_spectrum_bin *input,
                                                     size_t input_count,
                                                     double power_gain_db,
                                                     double output_p1db_dbm,
                                                     double output_saturation_dbm,
                                                     double input_ip2_dbm,
                                                     double input_ip3_dbm,
                                                     double reference_ohms,
                                                     rfmodel_amplifier_component *output,
                                                     size_t capacity,
                                                     size_t *output_count,
                                                     rfmodel_amplifier_drive *drive);

typedef struct rfmodel_incident_spectrum {
    double spacing_hz;
    const rfmodel_spectrum_bin *bins;
    size_t count;
} rfmodel_incident_spectrum;

/* One incident spectrum per physical port, 1..1024 ports, at most 2048 bins each.
 * Same-port coherent paths must already be combined. Nonzero DC is rejected.
 * Select an RF bin > 0 on fundamental_port; absent bin returns zero after validation.
 * No harmonic generation or feedback solve. Both outputs are required and unchanged on failure. */
RFMODEL_API int rfmodel_p1db_spectral_fundamental(double power_gain_db,
                                                  double output_p1db_dbm,
                                                  const rfmodel_incident_spectrum *ports,
                                                  size_t port_count,
                                                  size_t fundamental_port,
                                                  int fundamental_bin,
                                                  rfmodel_complex *output,
                                                  double *total_incident_power_w);

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

/* y(t) = sum coefficients[n] * v(t)^n, voltage coefficients, degree 0..11.
 * Input/output are RMS power waves at reference_ohms; DC is retained.
 * Uses the same spectrum buffer contract as the cubic amplifier. */
RFMODEL_API int rfmodel_polynomial_amplifier_transmit(double spacing_hz,
                                                      const rfmodel_spectrum_bin *input,
                                                      size_t input_count,
                                                      const double *coefficients,
                                                      size_t coefficient_count,
                                                      double reference_ohms,
                                                      rfmodel_spectrum_bin *output,
                                                      size_t capacity,
                                                      size_t *output_count);

/* Equal-tone extrapolated IIP2/IIP3; positive quadratic and negative cubic.
 * Generates DC and all products; no independent P1dB/saturation calibration. */
RFMODEL_API int rfmodel_intercept_amplifier_transmit(double spacing_hz,
                                                     const rfmodel_spectrum_bin *input,
                                                     size_t input_count,
                                                     double power_gain_db,
                                                     double input_ip2_dbm,
                                                     double input_ip3_dbm,
                                                     double reference_ohms,
                                                     rfmodel_spectrum_bin *output,
                                                     size_t capacity,
                                                     size_t *output_count);
/* One nonzero RF tone only. Independent saturated fundamental and soft-limited H2/H3.
 * DC is blocked; nonzero DC/multitone input is rejected. Empirical limiter offsets
 * are -4 dB from OP1dB-G and -1 dB from OPSAT-G. Not a full RFAMP/feedback model.
 * Uses the same spectrum buffer/count contract as the other transmit functions. */
RFMODEL_API int rfmodel_single_tone_amplifier_transmit(double spacing_hz,
                                                       const rfmodel_spectrum_bin *input,
                                                       size_t input_count,
                                                       double power_gain_db,
                                                       double output_p1db_dbm,
                                                       double output_saturation_dbm,
                                                       double input_ip2_dbm,
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
