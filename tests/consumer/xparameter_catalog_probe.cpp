#include <rfmodel/io/xparameter_catalog.hpp>
#include <sstream>

rfmodel::XParameterCatalog xparameter_catalog_probe() {
    std::istringstream input(
        "BEGIN XParamAttributes\n% Version(real) NumPorts(int) NumFundFreqs(int)\n2 3 2\nEND\n"
        "VAR AN_1_1_0(real) = 10\nBEGIN XParamData\n% AN_2_0_1(real)\n"
        "% FB_3_1_m2(complex)\n0\n-2.5 3.5\n1\n4 -5\nEND\n");
    return rfmodel::XParameterCatalog(rfmodel::read_xparameter_data(input));
}
