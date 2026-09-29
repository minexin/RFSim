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

typedef struct rfmodel_complex {
    double real;
    double imag;
} rfmodel_complex;

enum rfmodel_status {
    RFMODEL_OK = 0,
    RFMODEL_INVALID_ARGUMENT = 1,
    RFMODEL_SOLVER_ERROR = 2,
    RFMODEL_OUT_OF_MEMORY = 3,
    RFMODEL_INTERNAL_ERROR = 4
};

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

#ifdef __cplusplus
}
#endif
#endif
