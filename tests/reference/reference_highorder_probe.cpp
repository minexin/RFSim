// Developer-only, one-request transport for archived high-order evidence replay.
// This deliberately is not an SDK parser or a general-purpose model interface.
#include <rfmodel/coherent/coherent_amplifier.hpp>
#include <rfmodel/coherent/coherent_highorder_amplifier.hpp>
#include <rfmodel/coherent/coherent_polynomial.hpp>
#include <rfmodel/nonlinear/polynomial_intercepts.hpp>
#include <rfmodel/nonlinear/am_pm.hpp>

#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <system_error>

#ifndef RFMODEL_NATIVE_REPLAY_BUILD_ID
#error "Build the developer probe with its manifest's RFMODEL_NATIVE_REPLAY_BUILD_ID"
#endif

namespace {
constexpr std::size_t request_limit = 65536;
constexpr std::size_t response_limit = 16 * 1024 * 1024;
constexpr std::size_t token_limit = 128;
constexpr const char *magic = "RFMODEL_NATIVE_REPLAY";
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

std::vector<double> coefficients(Request &request, std::size_t maximum, std::size_t minimum = 0) {
    const auto count = request.count(maximum, minimum);
    std::vector<double> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(request.real());
    }
    return result;
}

std::vector<rfmodel::CoherentComponent> components(Request &request) {
    const auto count = request.count(2);
    std::vector<rfmodel::CoherentComponent> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto bin = request.integer<std::int32_t>();
        const auto kind = request.integer<int>();
        const auto bandwidth = request.real();
        const auto group = request.integer<std::uint64_t>();
        const auto real = request.real();
        const auto imaginary = request.real();
        if (bin <= 0 || kind != 0 || bandwidth <= 0. || group == 0) {
            throw std::invalid_argument("source component");
        }
        result.push_back({bin, rfmodel::SpectrumKind::source, bandwidth, group, {real, imaginary}});
    }
    return result;
}

void finite(double value) {
    if (!std::isfinite(value)) {
        throw std::overflow_error("nonfinite result");
    }
}

void write_point(std::ostream &out, const rfmodel::AmplifierOperatingPoint &point) {
    const double values[]{point.fundamental_amplitude_gain,
                          point.nonlinear_input_scale,
                          point.limited_input_power_w,
                          point.quadratic_voltage_coefficient,
                          point.cubic_voltage_coefficient};
    for (double value : values) {
        finite(value);
        out << value << ' ';
    }
    if (values[0] < 0 || values[1] < 0 || values[2] < 0) {
        throw std::runtime_error("invalid operating point result");
    }
    out << '\n';
}

void write_component(std::ostream &out,
                     const rfmodel::CoherentComponent &component,
                     double spacing) {
    finite(component.bandwidth_hz);
    finite(component.amplitude.real());
    finite(component.amplitude.imag());
    finite(std::norm(component.amplitude));
    finite(component.bin * spacing);
    finite(component.bin * spacing + component.bandwidth_hz / 2.);
    if (component.bin <= 0 || component.kind < rfmodel::SpectrumKind::source ||
        component.kind > rfmodel::SpectrumKind::intermod || component.bandwidth_hz <= 0 ||
        component.coherence_group == 0 ||
        component.bin * spacing - component.bandwidth_hz / 2. < 0) {
        throw std::runtime_error("invalid component result");
    }
    out << component.bin << ' ' << static_cast<int>(component.kind) << ' ' << component.bandwidth_hz
        << ' ' << component.coherence_group << ' ' << component.amplitude.real() << ' '
        << component.amplitude.imag() << '\n';
}

template <class Response>
void write_response(std::ostream &out, const Response &result, double spacing) {
    if (result.inputs.size() > 2 || result.terms.size() > 4096) {
        throw std::length_error("result count limit");
    }
    out << result.inputs.size() << '\n';
    for (const auto &component : result.inputs) {
        write_component(out, component, spacing);
    }
    out << result.terms.size() << '\n';
    for (const auto &term : result.terms) {
        if (term.order < 1 || term.order > 11) {
            throw std::runtime_error("invalid term result");
        }
        out << term.order << ' ';
        for (int i = 0; i < term.order; ++i) {
            const auto index = term.input_indices[i];
            if (index == 0 || index < -static_cast<int>(result.inputs.size()) ||
                index > static_cast<int>(result.inputs.size())) {
                throw std::runtime_error("invalid result index");
            }
            out << index << ' ';
        }
        for (std::size_t i = static_cast<std::size_t>(term.order); i < term.input_indices.size();
             ++i) {
            if (term.input_indices[i] != 0) {
                throw std::runtime_error("invalid index padding");
            }
        }
        write_component(out, term.component, spacing);
    }
}

void execute(Request &request, const std::string &operation, std::ostream &out) {
    if (operation == "intercepts") {
        const auto gain = request.real();
        const auto reference = request.real();
        const auto count = request.count(10);
        std::vector<rfmodel::TwoToneIntercept> definitions;
        definitions.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto first = request.integer<int>();
            const auto second = request.integer<int>();
            const auto intercept = request.real();
            const auto sign = request.integer<int>();
            const auto ref = request.integer<int>();
            if (ref != 0 && ref != 1) {
                throw std::invalid_argument("intercept reference");
            }
            definitions.push_back(
                {first, second, intercept, sign, static_cast<rfmodel::InterceptReference>(ref)});
        }
        request.finish();
        const auto result =
            rfmodel::polynomial_coefficients_from_intercepts(gain, definitions, reference);
        if (result.empty() || result.size() > 12) {
            throw std::runtime_error("invalid coefficient result");
        }
        out << result.size() << '\n';
        for (double value : result) {
            finite(value);
            out << value << ' ';
        }
        out << '\n';
    } else if (operation == "am_pm") {
        const auto power = request.real();
        const auto peak = request.real();
        const auto reference = request.real();
        request.finish();
        const auto result = rfmodel::LegacyPointDerivativeAmPm(peak, reference).response(power);
        finite(result.phase_rad);
        finite(result.radial_phase_slope);
        out << result.phase_rad << ' ' << result.radial_phase_slope << '\n';
    } else if (operation == "operating_point") {
        const auto drive = request.real();
        const auto gain = request.real();
        const auto p1db = request.real();
        const auto saturation = request.real();
        const auto ip2 = request.real();
        const auto ip3 = request.real();
        const auto reference = request.real();
        request.finish();
        if (drive < 0.) {
            throw std::invalid_argument("negative drive");
        }
        write_point(out,
                    rfmodel::CoherentLimitedAmplifier(gain, p1db, saturation, ip2, ip3, reference)
                        .operating_point(drive));
    } else if (operation == "polynomial" || operation == "highorder") {
        const auto spacing = request.real();
        const auto reference = request.real();
        const auto reserved = request.integer<std::uint64_t>();
        if (operation == "polynomial") {
            const auto values = coefficients(request, 12, 1);
            const auto input = components(request);
            request.finish();
            const auto result =
                rfmodel::CoherentPolynomial(values, reference).evaluate(spacing, input, reserved);
            write_response(out, result, spacing);
        } else {
            const auto gain = request.real();
            const auto p1db = request.real();
            const auto saturation = request.real();
            const auto values = coefficients(request, 10);
            const auto input = components(request);
            request.finish();
            const auto result =
                rfmodel::CoherentHighOrderAmplifier(gain, p1db, saturation, values, reference)
                    .evaluate(spacing, input, reserved, false);
            finite(result.total_input_power_w);
            if (result.total_input_power_w < 0.) {
                throw std::runtime_error("negative result power");
            }
            out << result.total_input_power_w << '\n';
            write_point(out, result.operating_point);
            write_response(out, result, spacing);
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
        std::ostringstream response;
        response.imbue(std::locale::classic());
        response << std::setprecision(std::numeric_limits<double>::max_digits10);
        response << magic << " 1 " << operation << ' ' << request_id << ' ' << build_id << " ok\n";
        execute(request, operation, response);
        const auto bytes = response.str();
        if (bytes.size() > response_limit) {
            throw std::length_error("response bytes");
        }
        // Nothing is written until every request and result field is valid.
        std::cout << bytes;
        return std::cout ? 0 : 5;
    } catch (const std::invalid_argument &) {
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
        // Stable categories avoid leaking platform-specific exception text.
    }
    std::cout << magic << " 1 error " << request_id << ' ' << build_id << ' ' << status << '\n';
    return code;
}
