#include "rfmodel/tabulated_model.hpp"
#include <iomanip>
#include <iostream>
#include <locale>
#include <sstream>

int main(int argc, char** argv) {
    if (argc<2) {
        std::cerr << "Usage: rfmodel_cli model.sNp [frequency_hz ...]\n";
        return 2;
    }
    try {
        const auto data=rfmodel::read_touchstone(argv[1]);
        rfmodel::TabulatedSParameterModel model("input",data);
        auto frequencies=data.frequencies_hz;
        if (argc>2) {
            frequencies.clear();
            for (int i=2; i<argc; ++i) {
                std::istringstream input(argv[i]);
                input.imbue(std::locale::classic());
                double f;
                if (!(input>>f) || input.peek()!=std::char_traits<char>::eof() || !std::isfinite(f) || f<0)
                    throw std::invalid_argument("invalid frequency argument");
                if (!frequencies.empty() && f<=frequencies.back())
                    throw std::invalid_argument("frequency arguments must increase strictly");
                frequencies.push_back(f);
            }
        }
        // Complete validation before emitting any output, avoiding partial CSV on model errors.
        std::vector<rfmodel::SMatrix> results;
        for (double f : frequencies) results.push_back(model.s_parameters(f));
        std::cout.imbue(std::locale::classic());
        std::cout << std::setprecision(17)
                  << "frequency_hz,row_port,column_port,reference_ohms,s_real,s_imag\n";
        for (std::size_t i=0; i<frequencies.size(); ++i)
            for (std::size_t r=0; r<model.port_count(); ++r)
                for (std::size_t c=0; c<model.port_count(); ++c) {
                    const auto s=results[i](r,c);
                    std::cout << frequencies[i] << ',' << r+1 << ',' << c+1 << ','
                              << data.reference_impedance_ohms << ',' << s.real() << ',' << s.imag() << '\n';
                }
        if (!std::cout) throw std::runtime_error("CSV output failed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "rfmodel_cli: " << error.what() << '\n';
        return 1;
    }
}
