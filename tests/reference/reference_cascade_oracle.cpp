// Independent deterministic mathematical anchors, not a vendor oracle.
// No RFModel headers, implementation helpers, C ABI, JSON, or Python. Every row:
// name operation payload-token-count payload... expected-token-count expected...
// Expected tokens contain first/second raw stages, then canonical combined
// origins. Unlike the transport body, they omit implementation roots and stats;
// combined groups are ignored. Scalar compression/limiting and ordered signed
// Fourier products are computed in long double without any tested SDK routines.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using Wide = long double;
using Wave = std::complex<Wide>;

struct Tone {
    int bin;
    double bandwidth;
    std::uint64_t group;
    double real;
    double imaginary;
};

struct Model {
    double gain = 20.;
    double p1db = 20.;
    double saturation = 23.;
    double iip2 = 20.;
    double iip3 = 10.;
    double reference = 50.;
    std::uint64_t reserved = 0;
};

struct Point {
    Wide gain;
    Wide scale;
    Wide limited;
};

std::ostringstream stream() {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    return out;
}

void number(std::ostream &out, Wide value) {
    const auto rounded = static_cast<double>(value);
    if (!std::isfinite(rounded)) {
        throw std::runtime_error("oracle result not representable");
    }
    out << rounded;
}

void wave(std::ostream &out, Wave value) {
    number(out, value.real());
    out << ' ';
    number(out, value.imag());
}

std::size_t tokens(const std::string &value) {
    std::istringstream in(value);
    in.imbue(std::locale::classic());
    std::size_t count = 0;
    std::string token;
    while (in >> token) {
        ++count;
    }
    return count;
}

std::string row(const std::string &name,
                const std::string &operation,
                const std::string &payload,
                const std::string &expected) {
    auto out = stream();
    out << name << ' ' << operation << ' ' << tokens(payload) << ' ' << payload << ' '
        << tokens(expected) << ' ' << expected;
    return out.str();
}

Point operating_point(const Model &m, Wide power) {
    // OP1dB is actual compressed output. The input anchor is OP1dB-G+1 dB.
    const Wide linear_gain = std::pow(Wide(10), Wide(m.gain) / 20);
    const Wide compression_at_anchor = std::pow(Wide(10), -Wide(1) / 20);
    const Wide input_anchor_power = std::pow(Wide(10), (Wide(m.p1db) - m.gain + 1 - 30) / 10);
    Wide gain = linear_gain * (1 - (1 - compression_at_anchor) * power / input_anchor_power);
    if (power > input_anchor_power) {
        const Wide output_anchor = std::sqrt(std::pow(Wide(10), (Wide(m.p1db) - 30) / 10));
        const Wide ceiling = std::sqrt(std::pow(Wide(10), (Wide(m.saturation) - 30) / 10));
        const Wide headroom = ceiling - output_anchor;
        const Wide slope = linear_gain * (3 * compression_at_anchor - 2);
        const Wide argument = slope * (std::sqrt(power) - std::sqrt(input_anchor_power)) / headroom;
        gain = (output_anchor + headroom * std::tanh(argument)) / std::sqrt(power);
    }
    // The nonlinear source limiter is a distinct scalar map from fundamental
    // compression. Its anchor and ceiling are fixed by this limited model.
    const Wide knee = std::sqrt(std::pow(Wide(10), (Wide(m.p1db) - m.gain - 34) / 10));
    const Wide ceiling = std::sqrt(std::pow(Wide(10), (Wide(m.saturation) - m.gain - 31) / 10));
    const Wide amplitude = std::sqrt(power);
    const Wide limited =
        amplitude <= knee
            ? amplitude
            : knee + (ceiling - knee) * std::tanh((amplitude - knee) / (ceiling - knee));
    return {gain, power == 0 ? 1 : limited / amplitude, limited * limited};
}

void component(
    std::ostream &out, int bin, int kind, double bandwidth, std::uint64_t group, Wave amplitude) {
    out << bin << ' ' << kind << ' ' << bandwidth << ' ' << group << ' ';
    wave(out, amplitude);
}


// Canonical primitive factors are signed ORIGINAL source ordinals. They never
// cancel: {-1,1,1} is a third-order origin, not the first-order source {1}.
struct Record {
    int bin, kind;
    double bandwidth;
    std::uint64_t group;
    Wave amplitude;
    std::vector<int> factors;
};
struct RawTerm { int order; std::vector<int> indices; Record component; };
struct Stage { Wide power, limited; std::vector<Record> inputs; std::vector<RawTerm> terms; };
using ProductKey = std::tuple<int, int, std::vector<int>>;
Stage evaluate(std::vector<Record> input, double gain) {
    std::sort(input.begin(), input.end(), [](const Record &a, const Record &b) {
        return std::tie(a.bin,a.kind,a.bandwidth,a.group)<std::tie(b.bin,b.kind,b.bandwidth,b.group);
    });
    Stage result{0,0,input,{}};
    std::uint64_t group=0;
    for (const auto &c: input) { result.power+=std::norm(c.amplitude); group=std::max(group,c.group); }
    const Model model{gain,20.,23.,40.-gain,30.-gain,50.,0};
    const auto op=operating_point(model,result.power);
    result.limited=op.limited;
    for (std::size_t i=0;i<input.size();++i) {
        auto c=input[i]; c.amplitude*=op.gain;
        result.terms.push_back({1,{static_cast<int>(i+1)},c});
    }
    const Wide voltage_scale=std::sqrt(Wide(25));
    const Wide linear_gain=std::pow(Wide(10),Wide(gain)/20);
    const Wide ip2=std::pow(Wide(10),(Wide(40)-gain-30)/10);
    const Wide ip3=std::pow(Wide(10),(Wide(30)-gain-30)/10);
    const std::array<Wide,4> coefficients{0,0,linear_gain/std::sqrt(100*ip2),-linear_gain/(75*ip3)};
    std::map<ProductKey,Wave> buckets;
    for (const int degree: {2,3}) {
        std::function<void(int,int,std::vector<int>,Wave)> visit;
        visit=[&](int depth,int bin,std::vector<int> indices,Wave voltage) {
            if (depth==degree) {
                if (bin<=0 || voltage==Wave{}) return;
                std::sort(indices.begin(),indices.end());
                buckets[{degree,bin,indices}]+=coefficients[static_cast<std::size_t>(degree)]*voltage/voltage_scale;
                return;
            }
            // Every ORDERED signed tuple, not the implementation's multiplicity formulas.
            for (std::size_t i=0;i<input.size();++i) {
                const auto &c=input[i];
                if(c.kind!=0 || c.amplitude==Wave{}) continue;
                for(const int sign: {-1,1}) {
                    auto next=indices;next.push_back(sign*static_cast<int>(i+1));
                    visit(depth+1,bin+sign*c.bin,next,voltage*op.scale*voltage_scale*
                        (sign>0 ? c.amplitude : std::conj(c.amplitude)));
                }
            }
        };
        visit(0,0,{},Wave{1,0});
    }
    for (const auto &bucket:buckets) {
        const auto &indices=std::get<2>(bucket.first);
        std::vector<int> factors;
        double bandwidth=0;
        for(int index:indices) {
            const auto &parent=input[static_cast<std::size_t>(std::abs(index)-1)];
            bandwidth+=parent.bandwidth;
            for(int factor:parent.factors) factors.push_back(index>0?factor:-factor);
        }
        std::sort(factors.begin(),factors.end());
        const int kind=indices.front()>0 && indices.front()==indices.back()?1:2;
        result.terms.push_back({std::get<0>(bucket.first),indices,
            {std::get<1>(bucket.first),kind,bandwidth,++group,bucket.second,factors}});
    }
    return result;
}
void output_component(std::ostream &out,const Record &c) {
    component(out,c.bin,c.kind,c.bandwidth,c.group,c.amplitude);
}
void output_stage(std::ostream &out,const Stage &s) {
    number(out,s.power); out<<' ';number(out,s.limited);out<<' '<<s.inputs.size();
    for(const auto &c:s.inputs){out<<' ';output_component(out,c);}
    out<<' '<<s.terms.size();
    for(const auto &t:s.terms){out<<' '<<t.order;for(int i:t.indices)out<<' '<<i;out<<' ';output_component(out,t.component);}
}
std::string cascade_case(const std::string &name,std::vector<Tone> tones,double gain) {
    auto request=stream();request<<1.e8<<' '<<gain<<' '<<tones.size();
    std::vector<Record> input;
    for(std::size_t i=0;i<tones.size();++i){const auto &t=tones[i];request<<' ';
        component(request,t.bin,0,t.bandwidth,t.group,{t.real,t.imaginary});
        input.push_back({t.bin,0,t.bandwidth,t.group,{t.real,t.imaginary},{static_cast<int>(i+1)}});
    }
    const auto first=evaluate(input,10.);
    input.clear();for(const auto &t:first.terms)input.push_back(t.component);
    const auto second=evaluate(input,gain);
    using Key=std::tuple<int,int,double,std::vector<int>>;
    std::map<Key,Record> combined;
    for(const auto &t:second.terms){const auto &c=t.component;const Key key{c.bin,c.kind,c.bandwidth,c.factors};
        auto found=combined.find(key);if(found==combined.end())combined.emplace(key,c);else found->second.amplitude+=c.amplitude;
    }
    auto expected=stream();output_stage(expected,first);expected<<' ';output_stage(expected,second);
    expected<<' '<<combined.size();
    for(const auto &entry:combined){const auto &c=entry.second;expected<<' '<<c.factors.size();
        for(int factor:c.factors){expected<<' '<<factor;}expected<<' ';output_component(expected,c);}
    return row(name,"cubic_two_stage_v1",request.str(),expected.str());
}
} // namespace
int main(int argc,char **) {
    if(argc!=1)return 2;
    try {
        std::vector<std::string> cases;
        const std::vector<std::array<double,4>> archive{
            {-20,90,1,0},{-20,90,1,0},{-10,45,1,10},{-10,45,1,10},
            {-30,0,1,10},{-30,0,1,10},{-30,90,2,10},{-10,90,2,10}};
        for(std::size_t i=0;i<archive.size();++i){const auto &a=archive[i];std::vector<Tone> tones;
            for(int j=0;j<static_cast<int>(a[2]);++j){const double phase=(a[2]==1||j==1?a[1]:0)*std::acos(-1.)/180.;
                const auto value=std::polar(std::sqrt(std::pow(10.,(a[0]-30.)/10.)),phase);
                tones.push_back({10+j,1.,static_cast<std::uint64_t>(j+1),value.real(),value.imag()});}
            cases.push_back(cascade_case("archive-"+std::to_string(i+1),tones,a[3]));
        }
        const std::vector<std::pair<std::string,std::vector<Tone>>> synthetic{
            {"reversed",{{11,1.,41,-.002,.001},{10,1.,7,.003,-.004}}},
            {"saturation",{{10,1.,17,.3,-.4},{11,1.,5,-.2,.1}}},
            {"unequal",{{10,1.,77,.03,-.004},{11,1.,99,-.0002,.001}}},
            {"phase-sign",{{10,1.,9,-.003,.004},{11,1.,5,.002,-.001}}}};
        for(const auto &item:synthetic)for(double gain:{0.,10.})
            cases.push_back(cascade_case(item.first+(gain==0?"-g0":"-g10"),item.second,gain));
        std::cout.imbue(std::locale::classic());std::cout<<"RFMODEL_CASCADE_ARCHIVE_ORACLE 1 "<<cases.size()<<'\n';
        for(const auto &c:cases){std::cout<<c<<'\n';}return std::cout?0:5;
    }catch(const std::exception &){std::cerr<<"cascade mathematical oracle failed\n";return 5;}
}
