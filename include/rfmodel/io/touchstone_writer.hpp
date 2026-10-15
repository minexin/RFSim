#pragma once
#include "rfmodel/io/touchstone.hpp"
#include <cstdint>
#include <cstring>
#include <memory>
#include <ostream>
#include <streambuf>

namespace rfmodel {
// Maximum serialized payload, including headers and [End]. Not configurable.
inline constexpr std::size_t touchstone_v2_max_output_bytes = 64u * 1024u * 1024u;

namespace touchstone_writer_detail {
struct NoisePolar {
    double magnitude;
    double degrees;
};

inline NoisePolar noise_polar(Complex gamma) {
    if (!std::isfinite(gamma.real()) || !std::isfinite(gamma.imag())) {
        throw std::invalid_argument("Touchstone noise reflection components must be finite");
    }
    // hypot avoids intermediate squaring overflow/underflow. A nonzero input
    // must not disappear when represented as magnitude; subnormals are allowed.
    const auto magnitude = std::hypot(gamma.real(), gamma.imag());
    const bool nonzero = gamma.real() != 0. || gamma.imag() != 0.;
    if (!std::isfinite(magnitude) || (nonzero && magnitude == 0.)) {
        throw std::invalid_argument("Touchstone noise reflection magnitude is not representable");
    }
    // atan2 preserves quadrants, including +/-180 for the signed imaginary zero
    // on the negative real axis. All complex zeros have canonical angle +0.
    constexpr double radians_to_degrees = 180. / 3.141592653589793238462643383279502884;
    const auto radians = nonzero ? std::atan2(gamma.imag(), gamma.real()) : 0.;
    const auto degrees = radians * radians_to_degrees;
    if (!std::isfinite(radians) || !std::isfinite(degrees) ||
        (gamma.imag() != 0. && radians == 0.) || (radians != 0. && degrees == 0.)) {
        throw std::invalid_argument("Touchstone noise reflection angle conversion is out of range");
    }
    return {magnitude, degrees};
}

// Geometric growth capped at the payload budget. Reallocation temporarily keeps
// the old allocation too (at most 96 MiB total payload storage at the final growth).
class Stage : public std::streambuf {
    std::unique_ptr<char[]> bytes_;
    std::size_t size_{};
    std::size_t capacity_{};

protected:
    std::streamsize xsputn(const char *data, std::streamsize count) override {
        if (count < 0 ||
            static_cast<std::uintmax_t>(count) > touchstone_v2_max_output_bytes - size_) {
            throw std::length_error("Touchstone output exceeds the 64 MiB byte budget");
        }
        const auto size = static_cast<std::size_t>(count);
        const auto required = size_ + size;
        if (required > capacity_) {
            auto capacity = capacity_ ? capacity_ : 1024;
            while (capacity < required) {
                capacity = std::min(touchstone_v2_max_output_bytes, capacity * 2);
            }
            std::unique_ptr<char[]> next(new char[capacity]);
            if (size_) {
                std::memcpy(next.get(), bytes_.get(), size_);
            }
            bytes_ = std::move(next);
            capacity_ = capacity;
        }
        if (size) {
            std::memcpy(bytes_.get() + size_, data, size);
            size_ += size;
        }
        return count;
    }

    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
        }
        const char byte = traits_type::to_char_type(value);
        xsputn(&byte, 1);
        return value;
    }

public:
    const char *data() const {
        return bytes_.get();
    }

    std::size_t size() const {
        return size_;
    }
};

[[noreturn]] inline void io_failure(std::ostream &output) {
    // setstate may itself throw according to the caller's unchanged exception mask.
    output.setstate(std::ios::badbit);
    throw std::ios_base::failure("Touchstone destination write failed");
}
} // namespace touchstone_writer_detail

// Serialize single-ended S data as Touchstone 2.0 / Hz / RI / Full, with optional
// raw two-port noise (Hz, Fmin dB, GammaOpt magnitude/degrees, physical Rn ohm).
// Representation metadata is ignored; matrices and frequencies are already SI/RI.
// No renormalization, model sampling, noise omission, or destination ownership.
// Invalid input, allocation/staging failure and budget overflow never write to output.
// Successful staging is followed by ONE unformatted streambuf write (no sentry,
// tied-stream flush, unitbuf flush, close, clear, or format/exception-mask changes).
// I/O failure throws with either exception-mask setting and may leave a prefix;
// success means accepted by the streambuf, not atomic replacement or durability.
inline void write_touchstone_v2(std::ostream &output, const TouchstoneData &data) {
    if (!data.noise_samples.empty() && data.ports != 2) {
        throw std::invalid_argument("Touchstone noise data requires two ports");
    }
    const auto references = effective_reference_impedances(data);
    if (data.frequencies_hz.empty() || data.frequencies_hz.size() != data.matrices.size()) {
        throw std::invalid_argument(
            "Touchstone requires matching nonempty frequencies and matrices");
    }
    // Port validation above bounds multiplication to 1024*1024.
    const auto entries = data.ports * data.ports;
    for (std::size_t i = 0; i < data.frequencies_hz.size(); ++i) {
        const auto frequency = data.frequencies_hz[i];
        if (!std::isfinite(frequency) || frequency < 0. ||
            (i && frequency <= data.frequencies_hz[i - 1])) {
            throw std::invalid_argument(
                "Touchstone frequencies must be finite, nonnegative and increasing");
        }
        const auto &matrix = data.matrices[i];
        if (matrix.ports != data.ports || matrix.values.size() != entries) {
            throw std::invalid_argument("Touchstone requires exact full N by N matrices");
        }
        for (const auto value : matrix.values) {
            if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
                throw std::invalid_argument("Touchstone matrix entries must be finite");
            }
        }
    }
    for (std::size_t i = 0; i < data.noise_samples.size(); ++i) {
        const auto &sample = data.noise_samples[i];
        if (!std::isfinite(sample.frequency_hz) || sample.frequency_hz < 0. ||
            (i ? sample.frequency_hz <= data.noise_samples[i - 1].frequency_hz
               : sample.frequency_hz > data.frequencies_hz.back())) {
            throw std::invalid_argument("invalid noise frequency order or first noise frequency");
        }
        if (!std::isfinite(sample.minimum_noise_figure_db) ||
            !std::isfinite(sample.noise_resistance_ohms) || sample.noise_resistance_ohms < 0.) {
            throw std::invalid_argument("noise requires finite Fmin and finite nonnegative Rn");
        }
        // Validate the MA conversion too, before staging or destination access.
        (void)touchstone_writer_detail::noise_polar(sample.optimum_source_reflection);
    }
    if (!output.rdbuf() || !output.good()) {
        throw std::ios_base::failure("Touchstone destination is not initially good");
    }
    touchstone_writer_detail::Stage buffer;
    std::ostream stage(&buffer);
    stage.imbue(std::locale::classic());
    stage.precision(std::numeric_limits<double>::max_digits10);
    stage.exceptions(std::ios::badbit | std::ios::failbit);
    stage << "[Version] 2.0\n# Hz S RI R " << data.reference_impedance_ohms
          << "\n[Number of Ports] " << data.ports << '\n';
    if (data.ports == 2) {
        stage << "[Two-Port Data Order] 12_21\n";
    }
    stage << "[Number of Frequencies] " << data.frequencies_hz.size() << '\n';
    if (!data.noise_samples.empty()) {
        stage << "[Number of Noise Frequencies] " << data.noise_samples.size() << '\n';
    }
    stage << "[Reference]";
    for (const auto reference : references) {
        stage << ' ' << reference.real();
    }
    stage << "\n[Matrix Format] Full\n[Network Data]\n";
    for (std::size_t i = 0; i < data.frequencies_hz.size(); ++i) {
        stage << data.frequencies_hz[i];
        for (std::size_t r = 0; r < data.ports; ++r) {
            for (std::size_t c = 0; c < data.ports; ++c) {
                const auto value = data.matrices[i](r, c);
                stage << ' ' << value.real() << ' ' << value.imag();
            }
            if (data.ports >= 3) {
                stage << '\n';
            }
        }
        if (data.ports < 3) {
            stage << '\n';
        }
    }
    if (!data.noise_samples.empty()) {
        stage << "[Noise Data]\n";
        for (const auto &sample : data.noise_samples) {
            const auto polar =
                touchstone_writer_detail::noise_polar(sample.optimum_source_reflection);
            // GammaOpt always uses option-line R; [Reference] is only for S.
            // Stored Rn is already ohms, even when source metadata says Legacy.
            stage << sample.frequency_hz << ' ' << sample.minimum_noise_figure_db << ' '
                  << polar.magnitude << ' ' << polar.degrees << ' ' << sample.noise_resistance_ohms
                  << '\n';
        }
    }
    stage << "[End]\n";
    if (static_cast<std::uintmax_t>(buffer.size()) >
        static_cast<std::uintmax_t>((std::numeric_limits<std::streamsize>::max)())) {
        throw std::length_error("Touchstone output does not fit streamsize");
    }
    const auto count = static_cast<std::streamsize>(buffer.size());
    std::streamsize written = 0;
    try {
        written = output.rdbuf()->sputn(buffer.data(), count);
    } catch (...) {
        touchstone_writer_detail::io_failure(output);
    }
    if (written != count || !output.good()) {
        touchstone_writer_detail::io_failure(output);
    }
}
} // namespace rfmodel
