// Developer-only, one-request transport for archived source-spectrum evidence replay.
// This is deliberately neither an SDK parser nor a general-purpose model API.
#include <rfmodel/coherent/coherent_amplifier.hpp>
#include <rfmodel/coherent/coherent_compression.hpp>
#include <rfmodel/coherent/source_coherence.hpp>

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
#include <tuple>
#include <vector>

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
void rfmodel_reference_source_odr_check();

namespace {
constexpr std::size_t request_limit = 65536;
constexpr std::size_t response_limit = 65536;
constexpr std::size_t token_limit = 128;
constexpr std::size_t output_count_limit = 16;
constexpr std::int32_t input_bin_limit = std::numeric_limits<std::int32_t>::max() / 3;
constexpr const char *magic = "RFMODEL_SOURCE_ARCHIVE_REPLAY";
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

bool identifier(const std::string &value) {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    const auto letter = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || digit(c) || c == '_';
    };
    if (!letter(value.front())) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [&](char c) {
        return letter(c) || c == '.' || c == ':' || c == '-';
    });
}

using ComponentKey = std::tuple<int, int, double, std::uint64_t>;

ComponentKey key(const rfmodel::CoherentComponent &component) {
    return {component.bin,
            static_cast<int>(component.kind),
            component.bandwidth_hz,
            component.coherence_group};
}

void validate_component(const rfmodel::CoherentComponent &c, double spacing, bool input) {
    finite(c.bandwidth_hz);
    finite(c.amplitude.real());
    finite(c.amplitude.imag());
    finite(std::norm(c.amplitude));
    if (c.amplitude != rfmodel::Complex{} && std::norm(c.amplitude) == 0.) {
        throw std::overflow_error("nonzero source wave power underflow");
    }
    const auto frequency = static_cast<double>(c.bin) * spacing;
    finite(frequency);
    finite(frequency + c.bandwidth_hz / 2.);
    if (input) {
        finite(3. * (frequency + c.bandwidth_hz / 2.));
    }
    if (c.bin <= 0 || (input && c.bin > input_bin_limit) ||
        c.kind < rfmodel::SpectrumKind::source || c.kind > rfmodel::SpectrumKind::intermod ||
        (input && c.kind != rfmodel::SpectrumKind::source) || c.bandwidth_hz <= 0. ||
        frequency - c.bandwidth_hz / 2. < 0. || c.coherence_group == 0 ||
        (input && c.coherence_group > std::numeric_limits<std::uint64_t>::max() - 16)) {
        throw std::invalid_argument("component support or source role");
    }
}

std::vector<rfmodel::CoherentComponent> read_components(Request &request, double spacing) {
    const auto count = request.count(2, 1);
    std::vector<rfmodel::CoherentComponent> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto bin = request.integer<std::int32_t>();
        const auto kind = request.integer<unsigned>();
        const auto bandwidth = request.real();
        const auto group = request.integer<std::uint64_t>();
        const auto real = request.real();
        const auto imaginary = request.real();
        if (kind != 0) {
            throw std::invalid_argument("source-only input");
        }
        result.push_back({bin, rfmodel::SpectrumKind::source, bandwidth, group, {real, imaginary}});
        validate_component(result.back(), spacing, true);
    }
    return result;
}

void write_component(std::ostream &out, const rfmodel::CoherentComponent &c, double spacing) {
    validate_component(c, spacing, false);
    out << c.bin << ' ' << static_cast<unsigned>(c.kind) << ' ' << c.bandwidth_hz << ' '
        << c.coherence_group << ' ';
    write_amplitude(out, c.amplitude);
    out << '\n';
}

void write_components(std::ostream &out,
                      const std::vector<rfmodel::CoherentComponent> &components,
                      double spacing) {
    if (components.empty() || components.size() > 2) {
        throw std::runtime_error("invalid reduced component count");
    }
    out << components.size() << '\n';
    for (std::size_t i = 0; i < components.size(); ++i) {
        if (components[i].kind != rfmodel::SpectrumKind::source ||
            (i && !(key(components[i - 1]) < key(components[i])))) {
            throw std::runtime_error("invalid reduced source ordering");
        }
        write_component(out, components[i], spacing);
    }
}

void nonnegative(double value) {
    finite(value);
    if (value < 0.) {
        throw std::runtime_error("invalid negative power");
    }
}

void write_terms(std::ostream &out,
                 const rfmodel::CoherentAmplifierResponse &result,
                 double spacing,
                 std::uint64_t reserved) {
    if (result.terms.size() < result.inputs.size() || result.terms.size() > output_count_limit) {
        throw std::runtime_error("invalid term count");
    }
    using TermKey = std::tuple<int, int, std::array<int, 3>>;
    std::set<TermKey> identities;
    std::set<std::uint64_t> generated_groups;
    std::uint64_t maximum = reserved;
    for (const auto &c : result.inputs) {
        maximum = std::max(maximum, c.coherence_group);
    }
    out << result.terms.size() << '\n';
    for (std::size_t position = 0; position < result.terms.size(); ++position) {
        const auto &term = result.terms[position];
        if (term.order < 1 || term.order > 3) {
            throw std::runtime_error("invalid term order");
        }
        long long bin = 0;
        double bandwidth = 0.;
        for (int i = 0; i < term.order; ++i) {
            const int index = term.input_indices[static_cast<std::size_t>(i)];
            if (index == 0 || index < -static_cast<int>(result.inputs.size()) ||
                index > static_cast<int>(result.inputs.size()) ||
                (i && term.input_indices[static_cast<std::size_t>(i - 1)] > index)) {
                throw std::runtime_error("invalid signed input index");
            }
            const auto &input = result.inputs[static_cast<std::size_t>(std::abs(index) - 1)];
            bin += index < 0 ? -static_cast<long long>(input.bin) : input.bin;
            bandwidth += input.bandwidth_hz;
        }
        for (int i = term.order; i < 3; ++i) {
            if (term.input_indices[static_cast<std::size_t>(i)] != 0) {
                throw std::runtime_error("nonzero unused input index");
            }
        }
        const auto expected_kind =
            term.order == 1 ? rfmodel::SpectrumKind::source
            : term.input_indices[0] > 0 &&
                    term.input_indices[0] ==
                        term.input_indices[static_cast<std::size_t>(term.order - 1)]
                ? rfmodel::SpectrumKind::harmonic
                : rfmodel::SpectrumKind::intermod;
        const TermKey identity{term.order, term.component.bin, term.input_indices};
        if (bin != term.component.bin || bandwidth != term.component.bandwidth_hz ||
            expected_kind != term.component.kind || !identities.emplace(identity).second ||
            (position && !(TermKey{result.terms[position - 1].order,
                                   result.terms[position - 1].component.bin,
                                   result.terms[position - 1].input_indices} < identity))) {
            throw std::runtime_error("invalid term identity or support");
        }
        if (term.order == 1) {
            const auto index = term.input_indices[0];
            if (index <= 0 ||
                key(term.component) != key(result.inputs[static_cast<std::size_t>(index - 1)])) {
                throw std::runtime_error("invalid direct identity");
            }
        } else if (term.component.coherence_group <= maximum ||
                   !generated_groups.emplace(term.component.coherence_group).second) {
            throw std::runtime_error("invalid generated group");
        }
        out << term.order;
        for (int i = 0; i < term.order; ++i) {
            out << ' ' << term.input_indices[static_cast<std::size_t>(i)];
        }
        out << ' ';
        write_component(out, term.component, spacing);
    }
    // Finite input tokens do not ensure representable derived products. The
    // SDK may omit a product that underflows to zero. This narrow transport
    // explicitly refuses that numeric domain rather than publishing an
    // incomplete origin family that could masquerade as a missing observation.
    std::set<TermKey> expected;
    std::vector<int> signed_inputs;
    for (std::size_t i = 0; i < result.inputs.size(); ++i) {
        const auto index = static_cast<int>(i + 1);
        expected.emplace(1, result.inputs[i].bin, std::array<int, 3>{index, 0, 0});
        if (result.inputs[i].amplitude != rfmodel::Complex{}) {
            signed_inputs.push_back(-index);
            signed_inputs.push_back(index);
        }
    }
    std::sort(signed_inputs.begin(), signed_inputs.end());
    const auto signed_bin = [&](int index) {
        const auto bin = result.inputs[static_cast<std::size_t>(std::abs(index) - 1)].bin;
        return index < 0 ? -bin : bin;
    };
    for (std::size_t i = 0; i < signed_inputs.size(); ++i) {
        for (std::size_t j = i; j < signed_inputs.size(); ++j) {
            const auto second_bin = signed_bin(signed_inputs[i]) + signed_bin(signed_inputs[j]);
            if (second_bin > 0) {
                expected.emplace(
                    2, second_bin, std::array<int, 3>{signed_inputs[i], signed_inputs[j], 0});
            }
            for (std::size_t k = j; k < signed_inputs.size(); ++k) {
                const auto third_bin = second_bin + signed_bin(signed_inputs[k]);
                if (third_bin > 0) {
                    expected.emplace(
                        3,
                        third_bin,
                        std::array<int, 3>{signed_inputs[i], signed_inputs[j], signed_inputs[k]});
                }
            }
        }
    }
    if (identities != expected) {
        throw std::overflow_error("source products underflow or incomplete family");
    }
}

void execute(Request &request, const std::string &operation, std::ostream &out) {
    if (operation == "source_groups_v1") {
        const auto count = request.count(2, 1);
        std::vector<rfmodel::SourceCoherence> definitions;
        std::set<std::string> names;
        for (std::size_t i = 0; i < count; ++i) {
            const auto source = request.token();
            const auto clock = request.token();
            if (!identifier(source) || (clock != "-" && !identifier(clock)) ||
                !names.emplace(source).second) {
                throw std::invalid_argument("source definition");
            }
            definitions.push_back({source, clock == "-" ? "" : clock});
        }
        request.finish();
        const auto groups = rfmodel::assign_source_coherence(definitions);
        if (groups.size() != definitions.size()) {
            throw std::runtime_error("invalid source groups");
        }
        out << groups.size() << '\n';
        for (std::size_t i = 0; i < groups.size(); ++i) {
            if (groups[i] == 0 || groups[i] > count) {
                throw std::runtime_error("source group bounds");
            }
            out << definitions[i].source_id << ' '
                << (definitions[i].reference_clock.empty() ? "-" : definitions[i].reference_clock)
                << ' ' << groups[i] << '\n';
        }
        return;
    }
    if (operation != "shared_fundamentals_v1" && operation != "cubic_sources_v1") {
        throw std::invalid_argument("unknown operation");
    }
    const auto spacing = request.real();
    const auto gain = request.real();
    const auto p1db = request.real();
    const auto saturation = request.real();
    if (spacing <= 0.) {
        throw std::invalid_argument("spectrum spacing");
    }
    if (operation == "shared_fundamentals_v1") {
        const auto input = read_components(request, spacing);
        request.finish();
        const auto result = rfmodel::compress_coherent_fundamentals(
            spacing, input, rfmodel::SaturatingFundamentalCompression(gain, p1db, saturation));
        nonnegative(result.input_power_w);
        nonnegative(result.output.total_power_w);
        out << result.input_power_w << '\n';
        write_components(out, result.output.components, spacing);
        if (result.output.power_by_bin_w.empty() || result.output.power_by_bin_w.size() > 2) {
            throw std::runtime_error("invalid bin power count");
        }
        out << result.output.power_by_bin_w.size() << '\n';
        for (const auto &entry : result.output.power_by_bin_w) {
            nonnegative(entry.second);
            out << entry.first << ' ' << entry.second << '\n';
        }
        out << result.output.total_power_w << '\n';
    } else {
        const auto iip2 = request.real();
        const auto iip3 = request.real();
        const auto reference = request.real();
        const auto reserved = request.integer<std::uint64_t>();
        if (reference <= 0. || reserved > std::numeric_limits<std::uint64_t>::max() - 16) {
            throw std::invalid_argument("reference impedance");
        }
        const auto input = read_components(request, spacing);
        request.finish();
        const auto result =
            rfmodel::CoherentLimitedAmplifier(gain, p1db, saturation, iip2, iip3, reference)
                .evaluate(spacing, input, reserved);
        nonnegative(result.total_input_power_w);
        nonnegative(result.limited_input_power_w);
        out << result.total_input_power_w << ' ' << result.limited_input_power_w << '\n';
        write_components(out, result.inputs, spacing);
        write_terms(out, result, spacing, reserved);
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
        rfmodel_reference_source_odr_check();
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
