"""Dispatch existing versioned analysis documents without recursive design tasks."""
from .model_file import analyze
from .spectrum_file import analyze_spectrum
from .amplifier_file import analyze_amplifier
from .coherence_file import analyze_coherence
from .coherent_network_file import analyze_coherent_network
from .coherent_system_file import analyze_coherent_system
from .mixer_linearization_file import analyze_mixer_linearization
from .conversion_file import analyze_conversion
from .conversion_network_file import analyze_conversion_network

_OPERATIONS = {
    'rfmodel.linear-network': analyze,
    'rfmodel.mixer-linearization': analyze_mixer_linearization,
    'rfmodel.conversion-network': analyze_conversion_network,
    'rfmodel.frequency-conversion': analyze_conversion,
    'rfmodel.coherence': analyze_coherence,
    'rfmodel.coherent-network': analyze_coherent_network,
    'rfmodel.coherent-system': analyze_coherent_system,
    'rfmodel.spectrum-chain': analyze_spectrum,
    'rfmodel.amplifier-components': analyze_amplifier,
}


def analysis_operation(document):
    """Validate only the dispatch tag; each analyzer validates its own schema."""
    if not isinstance(document, dict) or not isinstance(document.get('format'), str):
        raise ValueError('Analysis must be an object with a format string')
    if document['format'] not in _OPERATIONS:
        raise ValueError('Unsupported model analysis format: ' + document['format'])
    return _OPERATIONS[document['format']]


def analyze_document(library, document, *, base_directory=None):
    return analysis_operation(document)(library, document, base_directory=base_directory)
