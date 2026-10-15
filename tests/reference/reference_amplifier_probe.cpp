// Developer-only, one-request transport for archived amplifier evidence replay.
// This is deliberately neither an SDK parser nor a general-purpose model API.
#include <rfmodel/nonlinear/fundamental_compression.hpp>
#include <rfmodel/nonlinear/polynomial_amplifier.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#endif

#ifndef RFMODEL_NATIVE_REPLAY_BUILD_ID
#error "Build the developer probe with its manifest's RFMODEL_NATIVE_REPLAY_BUILD_ID"
#endif

// Defined in another translation unit, including the same public SDK headers.
// Both in-tree and installed consumers must actually compile and link that TU.
void rfmodel_reference_amplifier_odr_check();

namespace {
constexpr std::size_t request_limit = 65536;
constexpr std::size_t response_limit = 65536;
constexpr std::size_t token_limit = 128;
constexpr std::size_t output_count_limit = 16;
constexpr std::int32_t input_bin_limit = std::numeric_limits<std::int32_t>::max() / 3;
constexpr const char *magic = "RFMODEL_AMPLIFIER_REPLAY";
constexpr const char *build_id = RFMODEL_NATIVE_REPLAY_BUILD_ID;

bool whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

bool digit(char c) {
    return c >= '0' && c <= '9';
}

bool digest(const std::string &value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}

class Request {
    std::string bytes_;
    std::size_t cursor_{};

public:
    explicit Request(std::istream &input) {
        bytes_.reserve(request_limit);
        char c;
        while (input.get(c)) {
            if (bytes_.size() == request_limit) {
                throw std::length_error("request bytes");
            }
            if (static_cast<unsigned char>(c) > 127 || c == '\0') {
                throw std::invalid_argument("request ASCII");
            }
            bytes_.push_back(c);
        }
        if (input.bad()) {
            throw std::runtime_error("request I/O");
        }
    }

    std::string token() {
        while (cursor_ < bytes_.size() && whitespace(bytes_[cursor_])) {
            ++cursor_;
        }
        const auto begin = cursor_;
        while (cursor_ < bytes_.size() && !whitespace(bytes_[cursor_])) {
            if (cursor_ - begin == token_limit) {
                throw std::length_error("token bytes");
            }
            ++cursor_;
        }
        if (begin == cursor_) {
            throw std::invalid_argument("truncated request");
        }
        return bytes_.substr(begin, cursor_ - begin);
    }

    template <class T> T integer() {
        const auto value = token();
        T result{};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
            throw std::invalid_argument("integer syntax or range");
        }
        return result;
    }

    std::size_t count(std::size_t maximum, std::size_t minimum = 0) {
        const auto value = integer<std::uint64_t>();
        if (value > maximum) {
            throw std::length_error("count limit");
        }
        if (value < minimum) {
            throw std::invalid_argument("count minimum");
        }
        return static_cast<std::size_t>(value);
    }

    double real() {
        const auto value = token();
        std::size_t i = 0;
        if (value[i] == '-' || value[i] == '+') {
            ++i;
        }
        bool digits = false, nonzero = false;
        while (i < value.size() && digit(value[i])) {
            digits = true;
            nonzero = nonzero || value[i] != '0';
            ++i;
        }
        if (i < value.size() && value[i] == '.') {
            ++i;
            while (i < value.size() && digit(value[i])) {
                digits = true;
                nonzero = nonzero || value[i] != '0';
                ++i;
            }
        }
        if (!digits) {
            throw std::invalid_argument("decimal syntax");
        }
        if (i < value.size() && (value[i] == 'e' || value[i] == 'E')) {
            ++i;
            if (i < value.size() && (value[i] == '-' || value[i] == '+')) {
                ++i;
            }
            const auto start = i;
            while (i < value.size() && digit(value[i])) {
                ++i;
            }
            if (start == i) {
                throw std::invalid_argument("decimal exponent");
            }
        }
        if (i != value.size()) {
            throw std::invalid_argument("decimal syntax");
        }
        std::istringstream stream(value);
        stream.imbue(std::locale::classic());
        double result{};
        stream >> std::noskipws >> result;
        // libc++ retains a correctly rounded tiny value but sets failbit when
        // strtod reports ERANGE. Tininess before rounding can also yield exactly
        // min(), the smallest normal. Accept only nonzero results in that range
        // after strict grammar and complete consumption. Other failures,
        // overflow and nonzero decimal underflow to zero remain errors.
        const bool tiny_nonzero =
            result != 0. && std::abs(result) <= std::numeric_limits<double>::min();
        if (stream.bad() || !stream.eof() || (stream.fail() && !tiny_nonzero) ||
            !std::isfinite(result) || (result == 0. && nonzero)) {
            throw std::overflow_error("decimal range");
        }
        return result;
    }

    void finish() {
        while (cursor_ < bytes_.size() && whitespace(bytes_[cursor_])) {
            ++cursor_;
        }
        if (cursor_ != bytes_.size()) {
            throw std::invalid_argument("extra request data");
        }
    }
};

void finite(double value) {
    if (!std::isfinite(value)) {
        throw std::overflow_error("nonfinite result");
    }
}

void write_amplitude(std::ostream &out, rfmodel::Complex amplitude) {
    finite(amplitude.real());
    finite(amplitude.imag());
    finite(std::norm(amplitude));
    out << amplitude.real() << ' ' << amplitude.imag();
}

std::set<int> supported_bins(const rfmodel::PowerWaveSpectrum &input) {
    std::set<int> signed_bins;
    for (const auto &entry : input.amplitudes) {
        signed_bins.insert(entry.first);
        signed_bins.insert(-entry.first);
    }
    std::set<int> supported;
    std::set<int> previous{0};
    for (int order = 1; order <= 3; ++order) {
        std::set<int> next;
        for (int left : previous) {
            for (int right : signed_bins) {
                next.insert(left + right);
            }
        }
        for (int bin : next) {
            if (bin >= 0) {
                supported.insert(bin);
            }
        }
        previous = std::move(next);
    }
    return supported;
}

void execute(Request &request, const std::string &operation, std::ostream &out) {
    if (operation == "p1db_fundamental") {
        const auto gain = request.real();
        const auto p1db = request.real();
        const auto real = request.real();
        const auto imaginary = request.real();
        const auto mode = request.integer<unsigned>();
        const auto drive = request.real();
        request.finish();
        if ((mode != 0 && mode != 1) || drive < 0. || (mode == 0 && drive != 0.)) {
            throw std::invalid_argument("fundamental drive mode");
        }
        const rfmodel::P1dBFundamentalCompression model(gain, p1db);
        const rfmodel::Complex incident{real, imaginary};
        const auto result = mode == 0 ? model.transmit_fundamental(incident)
                                      : model.transmit_fundamental(incident, drive);
        write_amplitude(out, result);
        out << '\n';
    } else if (operation == "intercept_spectrum") {
        const auto spacing = request.real();
        const auto gain = request.real();
        const auto iip2 = request.real();
        const auto iip3 = request.real();
        const auto reference = request.real();
        const auto count = request.count(2, 1);
        if (spacing <= 0. || reference <= 0.) {
            throw std::invalid_argument("spectrum spacing or reference");
        }
        rfmodel::PowerWaveSpectrum incident{spacing, {}};
        int maximum_bin = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto bin = request.integer<std::int32_t>();
            const auto real = request.real();
            const auto imaginary = request.real();
            if (bin <= 0 || bin > input_bin_limit ||
                !std::isfinite(static_cast<double>(bin) * spacing) ||
                !incident.amplitudes.emplace(bin, rfmodel::Complex{real, imaginary}).second) {
                throw std::invalid_argument("spectrum source bin");
            }
            maximum_bin = std::max(maximum_bin, static_cast<int>(bin));
        }
        request.finish();
        const auto result = rfmodel::MatchedPolynomialAmplifier::from_intercepts(
                                "archived amplifier replay", gain, iip2, iip3, reference)
                                .transmit(incident);
        if (result.spacing_hz != spacing || result.amplitudes.size() > output_count_limit) {
            throw std::runtime_error("invalid spectrum result grid or size");
        }
        const auto support = supported_bins(incident);
        out << result.amplitudes.size() << '\n';
        int previous = -1;
        for (const auto &entry : result.amplitudes) {
            if (entry.first <= previous || entry.first > maximum_bin * 3 ||
                support.count(entry.first) == 0 ||
                (entry.first == 0 && entry.second.imag() != 0.)) {
                throw std::runtime_error("invalid spectrum result bin");
            }
            finite(static_cast<double>(entry.first) * spacing);
            out << entry.first << ' ';
            write_amplitude(out, entry.second);
            out << '\n';
            previous = entry.first;
        }
    } else {
        throw std::invalid_argument("unknown operation");
    }
}
} // namespace

int main(int argc, char **) {
    std::string request_id(64, '0');
    const char *status = "internal_error";
    int code = 5;
    try {
#ifdef _WIN32
        // Preserve raw protocol bytes: the CRT text mode folds CRLF and treats
        // Ctrl-Z as EOF. These changes affect only this process's stdio streams.
        const auto input_mode = _setmode(_fileno(stdin), _O_BINARY);
        const auto output_mode = _setmode(_fileno(stdout), _O_BINARY);
        if (input_mode == -1 || output_mode == -1) {
            throw std::runtime_error("stdio binary mode");
        }
#endif
        if (argc != 1 || !digest(build_id)) {
            throw std::invalid_argument("invocation");
        }
        Request request(std::cin);
        if (request.token() != magic || request.integer<unsigned>() != 1) {
            throw std::invalid_argument("protocol version");
        }
        const auto operation = request.token();
        const auto id = request.token();
        if (!digest(id)) {
            throw std::invalid_argument("request identity");
        }
        request_id = id;
        rfmodel_reference_amplifier_odr_check();
        std::ostringstream response;
        response.imbue(std::locale::classic());
        response << std::setprecision(std::numeric_limits<double>::max_digits10);
        response << magic << " 1 " << operation << ' ' << request_id << ' ' << build_id << " ok\n";
        execute(request, operation, response);
        const auto bytes = response.str();
        if (bytes.size() > response_limit) {
            throw std::length_error("response bytes");
        }
        // Never expose a partial result or echo malformed input/exception text.
        std::cout << bytes;
        return std::cout ? 0 : 5;
    } catch (const std::invalid_argument &) {
        status = "invalid_input";
        code = 2;
    } catch (const std::out_of_range &) {
        status = "invalid_input";
        code = 2;
    } catch (const std::length_error &) {
        status = "resource_limit";
        code = 3;
    } catch (const std::overflow_error &) {
        status = "numeric_range";
        code = 4;
    } catch (const std::bad_alloc &) {
        status = "resource_limit";
        code = 3;
    } catch (const std::exception &) {
        // Stable categories deliberately exclude platform-specific diagnostics.
    }
    std::cout << magic << " 1 error " << request_id << ' ' << build_id << ' ' << status << '\n';
    return code;
}
