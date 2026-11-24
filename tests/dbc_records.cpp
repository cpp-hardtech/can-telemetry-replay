#include "dbc/dbc_parser.h"

#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {

struct Callbacks {
    std::ostringstream records;

    friend void tag_invoke(can::def_sg_cpo, Callbacks& self, std::uint32_t id,
                           std::optional<unsigned>, std::string signal, unsigned, unsigned,
                           char, char, double factor, double, double, double, std::string,
                           std::vector<std::size_t>) {
        if (signal == "FineSpeed" || signal == "FineRate") {
            self.records << "scale " << id << ' ' << signal << ' ' << std::scientific
                         << std::setprecision(2) << factor << '\n';
        }
    }

    friend void tag_invoke(can::def_bo_tx_bu_cpo, Callbacks& self, unsigned id,
                           std::vector<std::string> senders) {
        self.records << "senders " << id;
        for (const auto& sender : senders) self.records << ' ' << sender;
        self.records << '\n';
    }

    friend void tag_invoke(can::def_sig_group_cpo, Callbacks& self, unsigned id, std::string group,
                           unsigned repetitions, std::vector<std::string> signals) {
        self.records << "group " << id << ' ' << group << ' ' << repetitions;
        for (const auto& signal : signals) self.records << ' ' << signal;
        self.records << '\n';
    }

    friend void tag_invoke(can::def_sig_valtype_cpo, Callbacks& self, unsigned id,
                           std::string signal, unsigned type) {
        self.records << "value type " << id << ' ' << signal << ' ' << type << '\n';
    }

    friend void tag_invoke(can::def_sgtype_cpo, Callbacks& self, std::string name, unsigned size,
                           char, char, double factor, double offset, double, double, std::string unit,
                           double default_value, std::size_t value_table) {
        self.records << "type " << name << ' ' << size << ' ' << factor << ' ' << offset
                     << ' ' << unit << ' ' << default_value << ' ' << value_table << '\n';
    }

    friend void tag_invoke(can::def_sgtype_ref_cpo, Callbacks& self, unsigned id,
                           std::string signal, std::string name) {
        self.records << "type ref " << id << ' ' << signal << ' ' << name << '\n';
    }

    friend void tag_invoke(can::def_ba_cpo, Callbacks& self, std::string name, std::string kind,
                           std::string object, std::size_t node, unsigned id,
                           std::variant<std::int32_t, double, std::string> value) {
        self.records << "attribute " << name << " kind=" << kind << " object=" << object
                     << " node=" << node << " id=" << id << " value=";
        std::visit([&](const auto& typed) { self.records << typed; }, value);
        self.records << '\n';
    }
};

bool expect(std::string_view section, std::string_view expected) {
    std::string dbc = "VERSION \"\"\n\nNS_ :\n\nBS_:\n\nBU_: Primary Backup Receiver\n\n";
    dbc += section;
    Callbacks callbacks;
    const bool success = can::parse_dbc(dbc, std::ref(callbacks));
    const auto actual = callbacks.records.str();
    std::cout << "Parsed: " << std::boolalpha << success << '\n' << actual;
    if (success && actual == expected) return true;
    std::cerr << "Expected callbacks:\n" << expected;
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string_view scenario = argv[1];
    if (scenario == "scales") {
        return expect(R"dbc(
BO_ 100 Telemetry: 4 Primary
 SG_ FineSpeed : 0|16@1+ (1e-18,0) [0|1] "m/s" Receiver
 SG_ FineRate : 16|16@1- (-1e-18,0) [-1|1] "m/s" Receiver
)dbc", "scale 100 FineSpeed 1.00e-18\nscale 100 FineRate -1.00e-18\n") ? 0 : 1;
    }
    if (scenario == "groups") {
        return expect(R"dbc(
BO_ 100 Telemetry: 2 Primary
 SG_ Speed : 0|8@1+ (1,0) [0|255] "kph" Receiver
 SG_ Mode : 8|8@1+ (1,0) [0|255] "" Receiver
BO_TX_BU_ 100 : Primary,Backup;
CM_ BO_ 100 "Vehicle dynamics";
VAL_ 100 Mode 0 "Stopped";
SIG_GROUP_ 100 Motion 2 : Speed Mode;
SIG_GROUP_ 100 Empty 1 : ;
SIG_VALTYPE_ 100 Speed : 0;
)dbc", "senders 100 Primary Backup\ngroup 100 Motion 2 Speed Mode\ngroup 100 Empty 1\n"
       "value type 100 Speed 0\n") ? 0 : 1;
    }
    if (scenario == "type-reference") {
        return expect(R"dbc(
VAL_TABLE_ TempTable 0 "Normal";
BO_ 100 Telemetry: 2 Primary
 SG_ Temperature : 0|16@1- (0.5,-40) [-40|80] "C" Receiver
SGTYPE_ Thermometer : 16@1- (0.5,-40) [-40|80] "C" 0, TempTable;
CM_ BO_ 100 "Temperature telemetry";
SGTYPE_ 100 Temperature : Thermometer;
)dbc", "type Thermometer 16 0.5 -40 C 0 0\ntype ref 100 Temperature Thermometer\n") ? 0 : 1;
    }
    if (scenario == "attributes") {
        return expect(R"dbc(
BO_ 100 Telemetry: 1 Primary
 SG_ Speed : 0|8@1+ (1,0) [0|255] "kph" Receiver
BA_DEF_ BU_ "Channel" INT 0 255;
BA_DEF_ SG_ "Label" STRING;
BA_DEF_ BO_ "Cycle" INT 1 1000;
BA_DEF_ "Project" STRING;
BA_DEF_DEF_ "Channel" 0;
BA_DEF_DEF_ "Label" "";
BA_DEF_DEF_ "Cycle" 100;
BA_DEF_DEF_ "Project" "";
BA_ "Channel" BU_ Backup 8;
BA_ "Label" SG_ 100 Speed "Road speed";
BA_ "Cycle" BO_ 100 50;
BA_ "Project" "Test fleet";
)dbc", "attribute Channel kind=BU_ object=Backup node=1 id=0 value=8\n"
       "attribute Label kind=SG_ object=Speed node=0 id=100 value=Road speed\n"
       "attribute Cycle kind=BO_ object= node=0 id=100 value=50\n"
       "attribute Project kind= object= node=0 id=0 value=Test fleet\n") ? 0 : 1;
    }
    return 2;
}
