#include <rfmodel/io/xparameter_data.hpp>
#include <sstream>

rfmodel::XParameterDocument xparameter_data_probe() {
    std::istringstream input("BEGIN PHDModelData\n% FB_2_1_m2(complex)\n-2.5 3.5\nEND\n");
    return rfmodel::read_xparameter_data(input);
}
