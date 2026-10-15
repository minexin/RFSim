#include "pll_noise.hpp"
#include <cmath>
#include <iomanip>
#include <iostream>
#include <locale>
#include <stdexcept>
#include <string>

int main(int argc, char **argv) {
    try {
        if (argc > 2 || (argc == 2 && std::string(argv[1]) != "--equal-loops")) {
            throw std::invalid_argument("usage: pll_phase_jitter [--equal-loops]");
        }
        // Exercise the public adapter's legal low-frequency grid and rejections.
        (void)example::simulate({1, 2}, false);
        for (const std::vector<int> &invalid : std::vector<std::vector<int>>{
                 {}, {100}, {0, 100}, {100, 100}, {200, 100}, {100, 100000000}}) {
            bool rejected = false;
            try {
                (void)example::simulate(invalid, false);
            } catch (const std::runtime_error &) {
                rejected = true;
            }
            if (!rejected) {
                throw std::runtime_error("invalid offset grid accepted");
            }
        }
        // User-chosen finite band, logarithmic sampling rounded to integer Hz.
        // Hz interval widths, not log-frequency widths, weight the integration.
        std::vector<int> offsets;
        for (int i = 0; i <= 80; ++i) {
            offsets.push_back(static_cast<int>(std::lround(100. * std::pow(10., i / 20.))));
        }
        const auto result = example::simulate(offsets, argc == 2);
        if (argc == 1) {
            // Independent fixed fixture at 10 kHz, from the unscaled closed form
            // H=N*(a*s+b)/(s*s+a*s+b), a=Kpd*Kvco*R/N, b=Kpd*Kvco/(N*C).
            // Check real and imaginary parts separately, so |H| or conjugating
            // the wrong output cannot hide behind the much larger real part.
            bool checked = false;
            for (const auto &row : result.offsets) {
                if (row.offset_hz == 10000.) {
                    const auto cross = row.cross_phase_rad2_per_hz;
                    if (std::abs(cross.real() / 2.0373096846266146e-12 - 1.) > 2e-11 ||
                        std::abs(cross.imag() / 3.911269270949205e-15 - 1.) > 2e-11) {
                        throw std::runtime_error("10 kHz complex cross-phase fixture failed");
                    }
                    checked = true;
                }
            }
            if (!checked) {
                throw std::runtime_error("default grid omitted the 10 kHz oracle fixture");
            }
        }
        std::cout.imbue(std::locale::classic());
        std::cout << std::scientific << std::setprecision(12) << "carrier_hz=" << result.carrier_hz
                  << " band_hz=[100,1000000] samples=" << result.offsets.size() << '\n'
                  << "offset_hz,H_ref_A_re,H_ref_A_im,H_vco_A_re,H_vco_A_im,reference_A_rad2_per_"
                     "hz,vco_A_rad2_per_hz,total_A_rad2_per_hz,"
                     "H_ref_B_re,H_ref_B_im,H_vco_B_re,H_vco_B_im,reference_B_rad2_per_hz,vco_B_"
                     "rad2_per_hz,total_B_rad2_per_hz,"
                     "cross_phase_real_rad2_per_hz,cross_phase_imag_rad2_per_hz,"
                     "difference_common_rad2_per_hz,difference_vco_rad2_per_hz\n";
        for (const auto &row : result.offsets) {
            std::cout << row.offset_hz;
            for (const auto &pll : row.pll) {
                std::cout << ',' << pll.transfer.reference.real() << ','
                          << pll.transfer.reference.imag() << ',' << pll.transfer.vco.real() << ','
                          << pll.transfer.vco.imag() << ',' << pll.noise.reference_rad2_per_hz
                          << ',' << pll.noise.vco_rad2_per_hz << ',' << pll.noise.total_rad2_per_hz;
            }
            std::cout << ',' << row.cross_phase_rad2_per_hz.real() << ','
                      << row.cross_phase_rad2_per_hz.imag() << ','
                      << row.differential_common_rad2_per_hz << ','
                      << row.differential_vco_rad2_per_hz << '\n';
        }
        for (std::size_t k = 0; k < 2; ++k) {
            const auto &j = result.jitter[k];
            std::cout << "output=" << k << " phase_variance_rad2=" << j.phase_variance_rad2
                      << " rms_phase_rad=" << j.rms_phase_rad << " rms_time_s=" << j.rms_time_s
                      << '\n';
        }
        std::cout << "All runtime oracles passed (also active in Release).\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
