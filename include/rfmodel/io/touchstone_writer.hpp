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

// Serialize single-ended S data as Touchstone 2.0 / Hz / RI / Full.
// Representation metadata is ignored; matrices and frequencies are already SI/RI.
// No renormalization, model sampling, noise omission, or destination ownership.
// Invalid input, allocation/staging failure and budget overflow never write to output.
// Successful staging is followed by ONE unformatted streambuf write (no sentry,
// tied-stream flush, unitbuf flush, close, clear, or format/exception-mask changes).
// I/O failure throws with either exception-mask setting and may leave a prefix;
// success means accepted by the streambuf, not atomic replacement or durability.
inline void write_touchstone_v2(std::ostream &output, const TouchstoneData &data) {
    if (!data.noise_samples.empty()) {
        throw std::invalid_argument("Touchstone v2 S writer does not support noise data");
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
    stage << "[Number of Frequencies] " << data.frequencies_hz.size() << "\n[Reference]";
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
