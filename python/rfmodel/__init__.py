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
            "rfmodel_passive_noise": (
                ct.c_int, [size, complex_pointer, size, ct.c_double, complex_pointer, size]),
            "rfmodel_transmission_line_s": (
                ct.c_int, [ct.c_double] * 5 + [complex_pointer, size]),
            "rfmodel_rlgc_line_s": (
                ct.c_int, [ct.c_double] * 7 + [complex_pointer, size]),
            "rfmodel_linear_amplifier_s": (
                ct.c_int, [ct.c_double] * 5 + [_Complex, _Complex, ct.c_double,
                                             complex_pointer, size]),
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
