"""Standard-library Python bindings for the RFModel linear-network C ABI."""
import ctypes as ct
import operator
from pathlib import Path
from threading import RLock
from typing import NamedTuple
import weakref


class RFModelError(RuntimeError):
    """A native RFModel error; status preserves the C ABI status code."""

    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


class Waves(NamedTuple):
    incident: tuple
    outgoing: tuple
    relative_residual: float


class LoadedNoise(NamedTuple):
    incident: tuple
    outgoing: tuple
    net_into_device_w_per_hz: tuple


class TouchstoneInfo(NamedTuple):
    ports: int
    reference_ohms: float
    minimum_frequency_hz: float
    maximum_frequency_hz: float
    noise_sample_count: int


class _TouchstoneInfo(ct.Structure):
    _fields_ = [("ports", ct.c_size_t), ("reference_ohms", ct.c_double),
                ("minimum_frequency_hz", ct.c_double), ("maximum_frequency_hz", ct.c_double),
                ("noise_sample_count", ct.c_size_t)]


class _Complex(ct.Structure):
    _fields_ = [("real", ct.c_double), ("imag", ct.c_double)]

    @classmethod
    def from_value(cls, value):
        value = complex(value)
        return cls(value.real, value.imag)

    def value(self):
        return complex(self.real, self.imag)


def _index(value):
    if isinstance(value, bool):
        raise TypeError("Port indices must be integers, not bool")
    value = operator.index(value)
    if not 0 <= value < 1024:
        raise ValueError("Port index must be in [0, 1023]")
    return value


class _SpectrumBin(ct.Structure):
    _fields_ = [("index", ct.c_int), ("amplitude", _Complex)]


class _IncidentSpectrum(ct.Structure):
    _fields_ = [("spacing_hz", ct.c_double), ("bins", ct.POINTER(_SpectrumBin)),
                ("count", ct.c_size_t)]


class DrivenFundamental(NamedTuple):
    amplitude: complex
    total_incident_power_w: float


def _bin(value):
    if isinstance(value, bool):
        raise TypeError("Frequency bin must be an integer")
    value = operator.index(value)
    if not 0 <= value <= 2147483647:
        raise ValueError("Frequency bin outside nonnegative signed 32-bit range")
    return value


def _spectrum(amplitudes):
    entries = list(amplitudes.items())
    if len(entries) > 2048:
        raise ValueError("Spectrum input exceeds 2048 bins")
    return (_SpectrumBin * len(entries))(
        *[_SpectrumBin(_bin(index), _Complex.from_value(value)) for index, value in entries])


def _matrix(values):
    rows = [list(row) for row in values]
    count = len(rows)
    if not 1 <= count <= 1024 or any(len(row) != count for row in rows):
        raise ValueError("Expected a square 1..1024 port matrix")
    return count, (_Complex * (count * count))(
        *[_Complex.from_value(value) for row in rows for value in row])


def _rows(values, count):
    return tuple(tuple(values[row * count + column].value() for column in range(count))
                 for row in range(count))


class Library:
    """Load an explicitly selected native library; never search implicit paths."""

    def __init__(self, path):
        self.path = Path(path).resolve(strict=True)
        self._dll = ct.CDLL(str(self.path))
        handle = ct.c_void_p
        size = ct.c_size_t
        complex_pointer = ct.POINTER(_Complex)
        signatures = {
            "rfmodel_last_error": (ct.c_char_p, []),
            "rfmodel_abi_version": (ct.c_uint, []),
            "rfmodel_saturating_fundamental": (
                ct.c_int, [ct.c_double, ct.c_double, ct.c_double, _Complex, ct.c_double, complex_pointer]),
            "rfmodel_p1db_fundamental": (
                ct.c_int, [ct.c_double, ct.c_double, _Complex, complex_pointer]),
            "rfmodel_p1db_driven_fundamental": (
                ct.c_int, [ct.c_double, ct.c_double, _Complex, ct.c_double, complex_pointer]),
            "rfmodel_p1db_spectral_fundamental": (
                ct.c_int, [ct.c_double, ct.c_double, ct.POINTER(_IncidentSpectrum), size,
                           size, ct.c_int, complex_pointer, ct.POINTER(ct.c_double)]),
            "rfmodel_touchstone_open": (ct.c_int, [ct.c_char_p, ct.c_int, ct.POINTER(handle)]),
            "rfmodel_touchstone_close": (None, [handle]),
            "rfmodel_touchstone_get_info": (ct.c_int, [handle, ct.POINTER(_TouchstoneInfo)]),
            "rfmodel_touchstone_s": (
                ct.c_int, [handle, ct.c_double, ct.c_double, complex_pointer, size]),
            "rfmodel_touchstone_noise": (
                ct.c_int, [handle, ct.c_double, ct.c_double, ct.c_double, complex_pointer, size]),
            "rfmodel_network_create": (ct.c_int, [ct.c_double, ct.POINTER(handle)]),
            "rfmodel_network_destroy": (None, [handle]),
            "rfmodel_network_add": (
                ct.c_int, [handle, size, complex_pointer, size, ct.c_double, ct.POINTER(size)]),
            "rfmodel_network_connect": (ct.c_int, [handle, size, size]),
            "rfmodel_network_terminate": (ct.c_int, [handle, size, _Complex, _Complex]),
            "rfmodel_network_port_count": (ct.c_int, [handle, ct.POINTER(size)]),
            "rfmodel_network_solve": (
                ct.c_int, [handle, complex_pointer, complex_pointer, size, ct.POINTER(ct.c_double)]),
            "rfmodel_network_external_s": (
                ct.c_int, [handle, ct.POINTER(size), size, complex_pointer, size]),
            "rfmodel_network_transmit_spectrum": (
                ct.c_int, [handle, ct.POINTER(size), size, ct.c_double,
                           ct.POINTER(_SpectrumBin), size, ct.POINTER(_SpectrumBin),
                           size, ct.POINTER(size)]),
            "rfmodel_passive_noise": (
                ct.c_int, [size, complex_pointer, size, ct.c_double, complex_pointer, size]),
            "rfmodel_loaded_noise": (
                ct.c_int, [size, complex_pointer, complex_pointer, complex_pointer, size,
                           complex_pointer, size, complex_pointer, complex_pointer, size,
                           ct.POINTER(ct.c_double), size]),
            "rfmodel_thermal_boundary_noise": (
                ct.c_int, [size, complex_pointer, ct.POINTER(ct.c_double), complex_pointer, size]),
            "rfmodel_transmission_line_s": (
                ct.c_int, [ct.c_double] * 5 + [complex_pointer, size]),
            "rfmodel_rlgc_line_s": (
                ct.c_int, [ct.c_double] * 7 + [complex_pointer, size]),
            "rfmodel_linear_amplifier_s": (
                ct.c_int, [ct.c_double] * 5 + [_Complex, _Complex, ct.c_double,
                                             complex_pointer, size]),
            "rfmodel_cubic_amplifier_transmit": (
                ct.c_int, [ct.c_double, ct.POINTER(_SpectrumBin), size] + [ct.c_double] * 3 +
                [ct.POINTER(_SpectrumBin), size, ct.POINTER(size)]),
            "rfmodel_polynomial_amplifier_transmit": (
                ct.c_int, [ct.c_double, ct.POINTER(_SpectrumBin), size, ct.POINTER(ct.c_double),
                           size, ct.c_double, ct.POINTER(_SpectrumBin), size, ct.POINTER(size)]),
            "rfmodel_intercept_amplifier_transmit": (
                ct.c_int, [ct.c_double, ct.POINTER(_SpectrumBin), size] + [ct.c_double] * 4 +
                [ct.POINTER(_SpectrumBin), size, ct.POINTER(size)]),
            "rfmodel_ideal_mixer_transmit": (
                ct.c_int, [ct.c_double, ct.POINTER(_SpectrumBin), size, ct.c_int] +
                [ct.c_double] * 3 + [ct.POINTER(_SpectrumBin), size, ct.POINTER(size)]),
            "rfmodel_single_tone_amplifier_transmit": (
                ct.c_int, [ct.c_double, ct.POINTER(_SpectrumBin), size] + [ct.c_double] * 6 +
                [ct.POINTER(_SpectrumBin), size, ct.POINTER(size)]),
            "rfmodel_network_external_noise": (
                ct.c_int, [handle, ct.POINTER(size), size, complex_pointer, size,
                           complex_pointer, size]),
        }
        for name, (result, arguments) in signatures.items():
            function = getattr(self._dll, name)
            function.restype = result
            function.argtypes = arguments
        if self._dll.rfmodel_abi_version() != 1:
            raise RuntimeError("Unsupported RFModel ABI version; expected 1")

    def _check(self, status):
        if status:
            message = self._dll.rfmodel_last_error().decode("utf-8", errors="replace")
            raise RFModelError(status, message)

    def network(self, reference_ohms=50.):
        return Network(self, reference_ohms)

    def p1db_fundamental(self, incident, *, power_gain_db, output_p1db_dbm,
                         total_incident_power_w=None):
        """Return fundamental in sqrt(W), optionally compressed by total incident RF power."""
        output = _Complex()
        arguments = (float(power_gain_db), float(output_p1db_dbm), _Complex.from_value(incident))
        if total_incident_power_w is None:
            status = self._dll.rfmodel_p1db_fundamental(*arguments, ct.byref(output))
        else:
            status = self._dll.rfmodel_p1db_driven_fundamental(
                *arguments, float(total_incident_power_w), ct.byref(output))
        self._check(status)
        return output.value()

    def saturating_fundamental(self, incident, *, power_gain_db, output_p1db_dbm,
                              output_saturation_dbm, total_incident_power_w=None):
        """Cubic / incremental-tanh fundamental response; no harmonic or AM/PM prediction."""
        incident = complex(incident)
        total = (incident.real * incident.real + incident.imag * incident.imag
                 if total_incident_power_w is None else float(total_incident_power_w))
        output = _Complex()
        self._check(self._dll.rfmodel_saturating_fundamental(
            float(power_gain_db), float(output_p1db_dbm), float(output_saturation_dbm),
            _Complex.from_value(incident), total, ct.byref(output)))
        return output.value()

    def touchstone(self, path, *, out_of_band="reject"):
        return Touchstone(self, path, out_of_band=out_of_band)

    def p1db_spectral_fundamental(self, ports, *, fundamental_port, fundamental_bin,
                                  power_gain_db, output_p1db_dbm):
        """Each port is (spacing_hz, bin-to-amplitude mapping); return amplitude and total W."""
        ports = list(ports)
        if not 1 <= len(ports) <= 1024:
            raise ValueError("Expected 1..1024 incident port spectra")
        fundamental_port = _index(fundamental_port)
        fundamental_bin = _bin(fundamental_bin)
        # Keep each ctypes array alive until the native call has consumed its pointer.
        buffers = [_spectrum(amplitudes) for _, amplitudes in ports]
        descriptors = (_IncidentSpectrum * len(ports))(
            *[_IncidentSpectrum(float(spacing), buffer, len(buffer))
              for (spacing, _), buffer in zip(ports, buffers)])
        output, total = _Complex(), ct.c_double()
        self._check(self._dll.rfmodel_p1db_spectral_fundamental(
            float(power_gain_db), float(output_p1db_dbm), descriptors, len(ports),
            fundamental_port, fundamental_bin, ct.byref(output), ct.byref(total)))
        return DrivenFundamental(output.value(), total.value)

    def thermal_boundary_noise(self, reflections, temperatures_k):
        reflections, temperatures = list(reflections), list(temperatures_k)
        count = len(reflections)
        if not 1 <= count <= 1024 or len(temperatures) != count:
            raise ValueError("Expected matching 1..1024 boundary reflections and temperatures")
        gamma = (_Complex * count)(*[_Complex.from_value(value) for value in reflections])
        temperature = (ct.c_double * count)(*[float(value) for value in temperatures])
        result = (_Complex * (count * count))()
        self._check(self._dll.rfmodel_thermal_boundary_noise(count, gamma, temperature,
                                                           result, len(result)))
        return _rows(result, count)

    def loaded_noise(self, scattering, intrinsic, reflections, boundary_emission):
        """Solve a=Gamma*b+e, b=S*a+c; intrinsic c and boundary e are independent."""
        count, s = _matrix(scattering)
        intrinsic_count, c = _matrix(intrinsic)
        boundary_count, e = _matrix(boundary_emission)
        reflections = list(reflections)
        if intrinsic_count != count or boundary_count != count or len(reflections) != count:
            raise ValueError("Loaded noise dimensions differ")
        gamma = (_Complex * count)(*[_Complex.from_value(value) for value in reflections])
        incident, outgoing = (_Complex * len(s))(), (_Complex * len(s))()
        power = (ct.c_double * count)()
        self._check(self._dll.rfmodel_loaded_noise(
            count, s, c, e, len(s), gamma, count, incident, outgoing, len(s), power, count))
        return LoadedNoise(_rows(incident, count), _rows(outgoing, count), tuple(power))

    def cubic_amplifier(self, spacing_hz, amplitudes, *, power_gain_db, input_ip3_dbm,
                        reference_ohms=50.):
        """Transmit sparse RMS power waves through a matched negative-cubic amplifier."""
        incident = _spectrum(amplitudes)
        output = (_SpectrumBin * 4096)()
        count = ct.c_size_t()
        self._check(self._dll.rfmodel_cubic_amplifier_transmit(
            float(spacing_hz), incident, len(incident), float(power_gain_db),
            float(input_ip3_dbm), float(reference_ohms), output, len(output), ct.byref(count)))
        return {output[i].index: output[i].amplitude.value() for i in range(count.value)}

    def intercept_amplifier(self, spacing_hz, amplitudes, *, power_gain_db, input_ip2_dbm,
                            input_ip3_dbm, reference_ohms=50.):
        """Positive quadratic/negative cubic calibrated from equal-tone input intercepts."""
        incident = _spectrum(amplitudes)
        output = (_SpectrumBin * 4096)()
        count = ct.c_size_t()
        self._check(self._dll.rfmodel_intercept_amplifier_transmit(
            float(spacing_hz), incident, len(incident), float(power_gain_db),
            float(input_ip2_dbm), float(input_ip3_dbm), float(reference_ohms),
            output, len(output), ct.byref(count)))
        return {output[i].index: output[i].amplitude.value() for i in range(count.value)}

    def single_tone_amplifier(self, spacing_hz, amplitudes, *, power_gain_db, output_p1db_dbm,
                              output_saturation_dbm, input_ip2_dbm, input_ip3_dbm, reference_ohms=50.):
        """Saturated fundamental plus soft-limited H2/H3; reject nonzero DC and multiple tones."""
        incident = _spectrum(amplitudes)
        output = (_SpectrumBin * 3)()
        count = ct.c_size_t()
        self._check(self._dll.rfmodel_single_tone_amplifier_transmit(
            float(spacing_hz), incident, len(incident), float(power_gain_db),
            float(output_p1db_dbm), float(output_saturation_dbm), float(input_ip2_dbm),
            float(input_ip3_dbm), float(reference_ohms), output, len(output), ct.byref(count)))
        return {output[i].index: output[i].amplitude.value() for i in range(count.value)}

    def polynomial_amplifier(self, spacing_hz, amplitudes, *, voltage_coefficients,
                             reference_ohms=50.):
        """Evaluate voltage polynomial degree 0..9; return all generated bins including DC."""
        coefficients = list(voltage_coefficients)
        if not 1 <= len(coefficients) <= 10:
            raise ValueError("Expected 1..10 voltage coefficients")
        native_coefficients = (ct.c_double * len(coefficients))(*map(float, coefficients))
        incident = _spectrum(amplitudes)
        output = (_SpectrumBin * 4096)()
        count = ct.c_size_t()
        self._check(self._dll.rfmodel_polynomial_amplifier_transmit(
            float(spacing_hz), incident, len(incident), native_coefficients, len(coefficients),
            float(reference_ohms), output, len(output), ct.byref(count)))
        return {output[i].index: output[i].amplitude.value() for i in range(count.value)}

    def ideal_mixer(self, spacing_hz, amplitudes, *, lo_bin, conversion_gain_db=0.,
                     lo_phase_radians=0., reference_ohms=50.):
        """Return both sidebands including coherent folding through zero frequency."""
        incident = _spectrum(amplitudes)
        output = (_SpectrumBin * 4096)()
        count = ct.c_size_t()
        self._check(self._dll.rfmodel_ideal_mixer_transmit(
            float(spacing_hz), incident, len(incident), _bin(lo_bin), float(conversion_gain_db),
            float(lo_phase_radians), float(reference_ohms), output, len(output), ct.byref(count)))
        return {output[i].index: output[i].amplitude.value() for i in range(count.value)}

    def passive_noise(self, scattering, temperature_k=290.):
        """Return k*T*(I-S*S^H) in W/Hz; active matrices are rejected."""
        count, values = _matrix(scattering)
        result = (_Complex * len(values))()
        self._check(self._dll.rfmodel_passive_noise(
            count, values, len(values), float(temperature_k), result, len(result)))
        return _rows(result, count)

    def transmission_line(self, frequency_hz, *, characteristic_ohms, delay_s,
                          propagation_loss_db=0., reference_ohms=50.):
        """Evaluate the C++ uniform line model; delay in seconds, loss in dB."""
        result = (_Complex * 4)()
        self._check(self._dll.rfmodel_transmission_line_s(
            float(frequency_hz), float(characteristic_ohms), float(delay_s),
            float(propagation_loss_db), float(reference_ohms), result, 4))
        return _rows(result, 2)

    def rlgc_line(self, frequency_hz, *, length_m, resistance_ohms_per_m=0.,
                  inductance_h_per_m=0., conductance_s_per_m=0., capacitance_f_per_m=0.,
                  reference_ohms=50.):
        """Evaluate the distributed C++ RLGC line, using per-metre coefficients."""
        result = (_Complex * 4)()
        self._check(self._dll.rfmodel_rlgc_line_s(
            float(frequency_hz), float(resistance_ohms_per_m), float(inductance_h_per_m),
            float(conductance_s_per_m), float(capacitance_f_per_m), float(length_m),
            float(reference_ohms), result, 4))
        return _rows(result, 2)

    def linear_amplifier(self, frequency_hz, *, gain_db=20., gain_phase_degrees=0.,
                         reverse_isolation_db=50., reverse_phase_degrees=0.,
                         input_impedance_ohms=50., output_impedance_ohms=50., reference_ohms=50.):
        """Bilateral small-signal S model; gain_db is 20*log10(abs(S21))."""
        result = (_Complex * 4)()
        self._check(self._dll.rfmodel_linear_amplifier_s(
            float(frequency_hz), float(gain_db), float(gain_phase_degrees),
            float(reverse_isolation_db), float(reverse_phase_degrees),
            _Complex.from_value(input_impedance_ohms), _Complex.from_value(output_impedance_ohms),
            float(reference_ohms), result, 4))
        return _rows(result, 2)


class Touchstone:
    """Owned immutable snapshot of a legacy .sNp file; context manager recommended."""

    def __init__(self, library, path, *, out_of_band="reject"):
        if out_of_band not in ("reject", "clamp"):
            raise ValueError("out_of_band must be reject or clamp")
        path = str(Path(path).resolve(strict=True))
        if "\x00" in path:
            raise ValueError("Path must not contain NUL")
        self._library = library
        self._lock = RLock()
        self._handle = ct.c_void_p()
        library._check(library._dll.rfmodel_touchstone_open(
            path.encode("utf-8"), int(out_of_band == "clamp"), ct.byref(self._handle)))
        self._finalizer = weakref.finalize(
            self, library._dll.rfmodel_touchstone_close, self._handle)

    def _open(self):
        if not self._finalizer.alive:
            raise RuntimeError("RFModel Touchstone model is closed")

    def close(self):
        with self._lock:
            self._finalizer()

    def __enter__(self):
        with self._lock:
            self._open()
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()

    @property
    def info(self):
        with self._lock:
            self._open()
            value = _TouchstoneInfo()
            self._library._check(self._library._dll.rfmodel_touchstone_get_info(
                self._handle, ct.byref(value)))
            return TouchstoneInfo(*(getattr(value, name) for name, _ in value._fields_))

    def s_parameters(self, frequency_hz, *, reference_ohms=None):
        with self._lock:
            info = self.info
            reference = info.reference_ohms if reference_ohms is None else float(reference_ohms)
            values = (_Complex * (info.ports * info.ports))()
            self._library._check(self._library._dll.rfmodel_touchstone_s(
                self._handle, float(frequency_hz), reference, values, len(values)))
            return _rows(values, info.ports)


    def noise_correlation(self, frequency_hz, *, reference_ohms=None, reference_temperature_k=290.):
        """Read embedded noise as W/Hz covariance; missing/invalid data is an error."""
        with self._lock:
            info = self.info
            reference = info.reference_ohms if reference_ohms is None else float(reference_ohms)
            values = (_Complex * (info.ports * info.ports))()
            self._library._check(self._library._dll.rfmodel_touchstone_noise(
                self._handle, float(frequency_hz), reference, float(reference_temperature_k),
                values, len(values)))
            return _rows(values, info.ports)


class Network:
    """Owned network handle. Use a with-block or close() for timely cleanup."""

    def __init__(self, library, reference_ohms=50.):
        self._library = library
        self._reference = float(reference_ohms)
        self._lock = RLock()
        self._handle = ct.c_void_p()
        library._check(library._dll.rfmodel_network_create(self._reference, ct.byref(self._handle)))
        self._finalizer = weakref.finalize(
            self, library._dll.rfmodel_network_destroy, self._handle)

    def _open(self):
        if not self._finalizer.alive:
            raise RuntimeError("RFModel network is closed")

    def close(self):
        with self._lock:
            self._finalizer()

    def __enter__(self):
        with self._lock:
            self._open()
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()

    @property
    def port_count(self):
        with self._lock:
            self._open()
            count = ct.c_size_t()
            self._library._check(self._library._dll.rfmodel_network_port_count(
                self._handle, ct.byref(count)))
            return count.value

    def add(self, scattering, reference_ohms=None):
        """Copy a square row-major nested sequence of complex S parameters."""
        with self._lock:
            self._open()
            count, values = _matrix(scattering)
            offset = ct.c_size_t()
            reference = self._reference if reference_ohms is None else float(reference_ohms)
            self._library._check(self._library._dll.rfmodel_network_add(
                self._handle, count, values, len(values), reference, ct.byref(offset)))
            return offset.value

    def connect(self, first, second):
        with self._lock:
            self._open()
            self._library._check(self._library._dll.rfmodel_network_connect(
                self._handle, _index(first), _index(second)))

    def terminate(self, port, reflection=0j, source=0j):
        with self._lock:
            self._open()
            self._library._check(self._library._dll.rfmodel_network_terminate(
                self._handle, _index(port), _Complex.from_value(reflection),
                _Complex.from_value(source)))

    def solve(self):
        with self._lock:
            count = self.port_count
            incident, outgoing = (_Complex * count)(), (_Complex * count)()
            residual = ct.c_double()
            self._library._check(self._library._dll.rfmodel_network_solve(
                self._handle, incident, outgoing, count, ct.byref(residual)))
            return Waves(tuple(value.value() for value in incident),
                         tuple(value.value() for value in outgoing), residual.value)

    def external_s(self, ports):
        with self._lock:
            self._open()
            indices = [_index(port) for port in ports]
            count = len(indices)
            if not 1 <= count <= 1024:
                raise ValueError("Select 1..1024 external ports")
            selection = (ct.c_size_t * count)(*indices)
            result = (_Complex * (count * count))()
            self._library._check(self._library._dll.rfmodel_network_external_s(
                self._handle, selection, count, result, len(result)))
            return tuple(tuple(result[row * count + column].value() for column in range(count))
                         for row in range(count))

    def transmit_spectrum(self, spacing_hz, amplitudes, external_ports):
        """Transmit fixed-S network with matched external input/output ports."""
        with self._lock:
            self._open()
            indices = [_index(port) for port in external_ports]
            if len(indices) != 2:
                raise ValueError("Select exactly two external ports, input then output")
            selection = (ct.c_size_t * 2)(*indices)
            incident = _spectrum(amplitudes)
            output = (_SpectrumBin * 4096)()
            count = ct.c_size_t()
            self._library._check(self._library._dll.rfmodel_network_transmit_spectrum(
                self._handle, selection, 2, float(spacing_hz), incident, len(incident),
                output, len(output), ct.byref(count)))
            return {output[i].index: output[i].amplitude.value() for i in range(count.value)}

    def external_noise(self, ports, intrinsic):
        """Propagate full global-port intrinsic covariance to matched external ports."""
        with self._lock:
            self._open()
            count, values = _matrix(intrinsic)
            if count != self.port_count:
                raise ValueError("Intrinsic covariance must cover all global network ports")
            indices = [_index(port) for port in ports]
            output_count = len(indices)
            if not 1 <= output_count <= 1024:
                raise ValueError("Select 1..1024 external ports")
            selection = (ct.c_size_t * output_count)(*indices)
            result = (_Complex * (output_count * output_count))()
            self._library._check(self._library._dll.rfmodel_network_external_noise(
                self._handle, selection, output_count, values, len(values), result, len(result)))
            return _rows(result, output_count)
