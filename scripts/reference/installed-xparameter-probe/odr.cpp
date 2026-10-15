// Independent translation unit exercises installed native stream definitions.
#ifdef RFMODEL_XPARAMETER_INSTALLED_HEADER
#include RFMODEL_XPARAMETER_INSTALLED_HEADER
#else
#include <rfmodel/io/xparameter_data.hpp>
#endif
#include <sstream>
namespace {
struct ReaderOdrCheck {
    ReaderOdrCheck() {
        std::istringstream input("BEGIN PHDModelData\n% value(real)\n1\nEND\n");
        if (rfmodel::read_xparameter_data(input).blocks().size() != 1) {
            throw std::runtime_error("installed reader ODR check failed");
        }
    }
};
const ReaderOdrCheck reader_odr_check{};
}
