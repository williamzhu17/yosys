/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Claire Xenia Wolf <claire@yosyshq.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

// [[CITE]] EDIF Version 2 0 0 Grammar
// http://web.archive.org/web/20050730021644/http://www.edif.org/documentation/BNF_GRAMMAR/index.html

#include "kernel/rtlil.h"
#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/celltypes.h"
#include "kernel/log.h"
#include <string>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

#define EDIF_DEF(_id) edif_names(RTLIL::unescape_id(_id), true).c_str()
#define EDIF_DEFR(_id, _ren, _bl, _br) edif_names(RTLIL::unescape_id(_id), true, _ren, _bl, _br).c_str()
#define EDIF_REF(_id) edif_names(RTLIL::unescape_id(_id), false).c_str()

struct EdifNames
{
	int counter;
	char delim_left, delim_right;
	std::set<std::string> generated_names, used_names;
	std::map<std::string, std::string> name_map;

	EdifNames() : counter(1), delim_left('['), delim_right(']') { }

	std::string operator()(std::string id, bool define, bool port_rename = false, int range_left = 0, int range_right = 0)
	{
		if (define) {
			std::string new_id = operator()(id, false);
			if (port_rename)
				return stringf("(rename %s \"%s%c%d:%d%c\")", new_id.c_str(), id.c_str(), delim_left, range_left, range_right, delim_right);
			return new_id != id ? stringf("(rename %s \"%s\")", new_id.c_str(), id.c_str()) : id;
		}

		if (name_map.count(id) > 0)
			return name_map.at(id);
		if (generated_names.count(id) > 0)
			goto do_rename;
		if (id == "GND" || id == "VCC")
			goto do_rename;

		for (size_t i = 0; i < id.size(); i++) {
			if ('A' <= id[i] && id[i] <= 'Z')
				continue;
			if ('a' <= id[i] && id[i] <= 'z')
				continue;
			if ('0' <= id[i] && id[i] <= '9' && i > 0)
				continue;
			if (id[i] == '_' && i > 0 && i != id.size()-1)
				continue;
			goto do_rename;
		}

		used_names.insert(id);
		return id;

	do_rename:;
		std::string gen_name;
		while (1) {
			gen_name = stringf("id%05d", counter++);
			if (generated_names.count(gen_name) == 0 &&
					used_names.count(gen_name) == 0)
				break;
		}
		generated_names.insert(gen_name);
		name_map[id] = gen_name;
		return gen_name;
	}
};

struct EdifBackend : public Backend {
	EdifBackend() : Backend("edif", "write design to EDIF netlist file") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    write_edif [options] [filename]\n");
		log("\n");
		log("Write the current design to an EDIF netlist file.\n");
		log("\n");
		log("    -top top_module\n");
		log("        set the specified module as design top module\n");
		log("\n");
		log("    -nogndvcc\n");
		log("        do not create \"GND\" and \"VCC\" cells. (this will produce an error\n");
		log("        if the design contains constant nets. use \"hilomap\" to map to custom\n");
		log("        constant drivers first)\n");
		log("\n");
		log("    -gndvccy\n");
		log("        create \"GND\" and \"VCC\" cells with \"Y\" outputs. (the default is\n");
		log("        \"G\" for \"GND\" and \"P\" for \"VCC\".)\n");
		log("\n");
		log("    -attrprop\n");
		log("        create EDIF properties for cell attributes\n");
		log("\n");
		log("    -keep\n");
		log("        create extra KEEP nets by allowing a cell to drive multiple nets.\n");
		log("\n");
		log("    -pvector {par|bra|ang}\n");
		log("        sets the delimiting character for module port rename clauses to\n");
		log("        parentheses, square brackets, or angle brackets.\n");
		log("\n");
		log("    -lsbidx\n");
		log("        use index 0 for the LSB bit of a net or port instead of MSB.\n");
		log("\n");
		log("Unfortunately there are different \"flavors\" of the EDIF file format. This\n");
		log("command generates EDIF files for the Xilinx place&route tools. It might be\n");
		log("necessary to make small modifications to this command when a different tool\n");
		log("is targeted.\n");
		log("\n");
	}
	void execute(std::ostream *&f, std::string filename, std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing EDIF backend.\n");
		std::string top_module_name;
		bool port_rename = false;
		bool attr_properties = false;
		bool lsbidx = false;
		std::map<RTLIL::IdString, std::map<RTLIL::IdString, int>> lib_cell_ports;
		bool nogndvcc = false, gndvccy = false, keepmode = false;
		CellTypes ct(design);
		EdifNames edif_names;

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++)
		{
			if (args[argidx] == "-top" && argidx+1 < args.size()) {
				top_module_name = args[++argidx];
				continue;
			}
			if (args[argidx] == "-nogndvcc") {
				nogndvcc = true;
				continue;
			}
			if (args[argidx] == "-gndvccy") {
				gndvccy = true;
				continue;
			}
			if (args[argidx] == "-attrprop") {
				attr_properties = true;
				continue;
			}
			if (args[argidx] == "-keep") {
				keepmode = true;
				continue;
			}
			if (args[argidx] == "-pvector" && argidx+1 < args.size()) {
				std::string parray;
				port_rename = true;
				parray = args[++argidx];
				if (parray == "par") {
					edif_names.delim_left = '(';edif_names.delim_right = ')';
				} else if (parray == "ang") {
					edif_names.delim_left = '<';edif_names.delim_right = '>';
				} else {
					edif_names.delim_left = '[';edif_names.delim_right = ']';
				}
				continue;
			}
			if (args[argidx] == "-lsbidx") {
				lsbidx = true;
				continue;
			}
			break;
		}
		extra_args(f, filename, args, argidx);

		if (top_module_name.empty())
			for (auto module : design->modules())
				if (module->get_bool_attribute(ID::top))
					top_module_name = module->name.str();

		for (auto module : design->modules())
		{
			lib_cell_ports[module->name];

			for (auto port : module->ports)
			{
				Wire *wire = module->wire(port);
				lib_cell_ports[module->name][port] = std::max(lib_cell_ports[module->name][port], GetSize(wire));
			}

			if (module->get_blackbox_attribute())
				continue;

			if (top_module_name.empty())
				top_module_name = module->name.str();

			if (module->processes.size() != 0)
				log_error("Found unmapped processes in module %s: unmapped processes are not supported in EDIF backend!\n", log_id(module->name));
			if (module->memories.size() != 0)
				log_error("Found unmapped memories in module %s: unmapped memories are not supported in EDIF backend!\n", log_id(module->name));

			for (auto cell : module->cells())
			{
				if (cell->type == ID($scopeinfo))
					continue;

				if (design->module(cell->type) == nullptr || design->module(cell->type)->get_blackbox_attribute()) {
					lib_cell_ports[cell->type];
					for (auto p : cell->connections())
						lib_cell_ports[cell->type][p.first] = std::max(lib_cell_ports[cell->type][p.first], GetSize(p.second));
				}
			}
		}

		if (top_module_name.empty())
			log_error("No module found in design!\n");

		*f << stringf("(edif %s\n", EDIF_DEF(top_module_name));
		*f << stringf("  (edifVersion 2 0 0)\n");
		*f << stringf("  (edifLevel 0)\n");
		*f << stringf("  (keywordMap (keywordLevel 0))\n");

		*f << stringf("  (comment \"Generated by %s\")\n", yosys_maybe_version());

		*f << stringf("  (external LIB\n");
		*f << stringf("    (edifLevel 0)\n");
		*f << stringf("    (technology (numberDefinition))\n");

		if (!nogndvcc)
		{
			*f << stringf("    (cell GND\n");
			*f << stringf("      (cellType GENERIC)\n");
			*f << stringf("      (view VIEW_NETLIST\n");
			*f << stringf("        (viewType NETLIST)\n");
			*f << stringf("        (interface (port %c (direction OUTPUT)))\n", gndvccy ? 'Y' : 'G');
			*f << stringf("      )\n");
			*f << stringf("    )\n");

			*f << stringf("    (cell VCC\n");
			*f << stringf("      (cellType GENERIC)\n");
			*f << stringf("      (view VIEW_NETLIST\n");
			*f << stringf("        (viewType NETLIST)\n");
			*f << stringf("        (interface (port %c (direction OUTPUT)))\n", gndvccy ? 'Y' : 'P');
			*f << stringf("      )\n");
			*f << stringf("    )\n");
		}

		for (auto &cell_it : lib_cell_ports) {
			*f << stringf("    (cell %s\n", EDIF_DEF(cell_it.first));
			*f << stringf("      (cellType GENERIC)\n");
			*f << stringf("      (view VIEW_NETLIST\n");
			*f << stringf("        (viewType NETLIST)\n");
			*f << stringf("        (interface\n");
			for (auto &port_it : cell_it.second) {
				const char *dir = "INOUT";
				if (ct.cell_known(cell_it.first)) {
					if (!ct.cell_output(cell_it.first, port_it.first))
						dir = "INPUT";
					else if (!ct.cell_input(cell_it.first, port_it.first))
						dir = "OUTPUT";
				}
				int width = port_it.second;
				int start = 0;
				bool upto = false;
				auto m = design->module(cell_it.first);
				if (m) {
					auto w = m->wire(port_it.first);
					if (w) {
						width = GetSize(w);
						start = w->start_offset;
						upto = w->upto;
					}
				}
				if (width == 1)
					*f << stringf("          (port %s (direction %s))\n", EDIF_DEF(port_it.first), dir);
				else {
					int b[2];
					b[upto ? 0 : 1] = start;
					b[upto ? 1 : 0] = start+width-1;
					*f << stringf("          (port (array %s %d) (direction %s))\n", EDIF_DEFR(port_it.first, port_rename, b[0], b[1]), width, dir);
				}
			}
			*f << stringf("        )\n");
			*f << stringf("      )\n");
			*f << stringf("    )\n");
		}
		*f << stringf("  )\n");

		std::vector<RTLIL::Module*> sorted_modules;

		// extract module dependencies
		std::map<RTLIL::Module*, std::set<RTLIL::Module*>> module_deps;
		for (auto module : design->modules()) {
			module_deps[module] = std::set<RTLIL::Module*>();
			for (auto cell : module->cells())
				if (design->module(cell->type) != nullptr)
					module_deps[module].insert(design->module(cell->type));
		}

		// simple good-enough topological sort
		// (O(n*m) on n elements and depth m)
		while (module_deps.size() > 0) {
			size_t sorted_modules_idx = sorted_modules.size();
			for (auto &it : module_deps) {
				for (auto &dep : it.second)
					if (module_deps.count(dep) > 0)
						goto not_ready_yet;
				// log("Next in topological sort: %s\n", log_id(it.first->name));
				sorted_modules.push_back(it.first);
			not_ready_yet:;
			}
			if (sorted_modules_idx == sorted_modules.size())
				log_error("Cyclic dependency between modules found! Cycle includes module %s.\n", log_id(module_deps.begin()->first->name));
			while (sorted_modules_idx < sorted_modules.size())
				module_deps.erase(sorted_modules.at(sorted_modules_idx++));
		}


		*f << stringf("  (library DESIGN\n");
		*f << stringf("    (edifLevel 0)\n");
		*f << stringf("    (technology (numberDefinition))\n");

		auto add_prop = [&](IdString name, Const val) {
			if ((val.flags & RTLIL::CONST_FLAG_STRING) != 0)
				*f << stringf("\n            (property %s (string \"%s\"))", EDIF_DEF(name), val.decode_string().c_str());
			else if (val.size() <= 32 && RTLIL::SigSpec(val).is_fully_def())
				*f << stringf("\n            (property %s (integer %u))", EDIF_DEF(name), val.as_int());
			else {
				std::string hex_string = "";
				for (auto i = 0; i < val.size(); i += 4) {
					int digit_value = 0;
					if (i+0 < val.size() && val.at(i+0) == RTLIL::State::S1) digit_value |= 1;
					if (i+1 < val.size() && val.at(i+1) == RTLIL::State::S1) digit_value |= 2;
					if (i+2 < val.size() && val.at(i+2) == RTLIL::State::S1) digit_value |= 4;
					if (i+3 < val.size() && val.at(i+3) == RTLIL::State::S1) digit_value |= 8;
					char digit_str[2] = { "0123456789abcdef"[digit_value], 0 };
					hex_string = std::string(digit_str) + hex_string;
				}
				*f << stringf("\n            (property %s (string \"%d'h%s\"))", EDIF_DEF(name), GetSize(val), hex_string.c_str());
			}
		};
		for (auto module : sorted_modules)
		{
			if (module->get_blackbox_attribute())
				continue;

			SigMap sigmap(module);
			std::map<RTLIL::SigSpec, std::set<std::pair<std::string, bool>>> net_join_db;

			*f << stringf("    (cell %s\n", EDIF_DEF(module->name));
			*f << stringf("      (cellType GENERIC)\n");
			*f << stringf("      (view VIEW_NETLIST\n");
			*f << stringf("        (viewType NETLIST)\n");
			*f << stringf("        (interface\n");

			for (auto cell : module->cells()) {
				for (auto &conn : cell->connections())
					if (cell->output(conn.first))
						sigmap.add(conn.second);
			}

			for (auto wire : module->wires())
				for (auto b1 : SigSpec(wire))
				{
					auto b2 = sigmap(b1);

					if (b1 == b2 || !b2.wire)
						continue;

					log_assert(b1.wire != nullptr);

					Wire *w1 = b1.wire;
					Wire *w2 = b2.wire;

					{
						int c1 = w1->get_bool_attribute(ID::keep);
						int c2 = w2->get_bool_attribute(ID::keep);

						if (c1 > c2) goto promote;
						if (c1 < c2) goto nopromote;
					}

					{
						int c1 = w1->name.isPublic();
						int c2 = w2->name.isPublic();

						if (c1 > c2) goto promote;
						if (c1 < c2) goto nopromote;
					}

					{
						auto count_nontrivial_attr = [](Wire *w) {
							int count = w->attributes.size();
							count -= w->attributes.count(ID::src);
							count -= w->attributes.count(ID::unused_bits);
							return count;
						};

						int c1 = count_nontrivial_attr(w1);
						int c2 = count_nontrivial_attr(w2);

						if (c1 > c2) goto promote;
						if (c1 < c2) goto nopromote;
					}

					{
						int c1 = w1->port_id ? INT_MAX - w1->port_id : 0;
						int c2 = w2->port_id ? INT_MAX - w2->port_id : 0;

						if (c1 > c2) goto promote;
						if (c1 < c2) goto nopromote;
					}

				nopromote:
					if (0)
				promote:
						sigmap.add(b1);
				}

			for (auto wire : module->wires()) {
				if (wire->port_id == 0)
					continue;
				const char *dir = "INOUT";
				if (!wire->port_output)
					dir = "INPUT";
				else if (!wire->port_input)
					dir = "OUTPUT";
				if (wire->width == 1) {
					*f << stringf("          (port %s (direction %s)", EDIF_DEF(wire->name), dir);
					if (attr_properties)
						for (auto &p : wire->attributes)
							add_prop(p.first, p.second);
					*f << ")\n";
					RTLIL::SigSpec sig = sigmap(RTLIL::SigSpec(wire));
					net_join_db[sig].insert(make_pair(stringf("(portRef %s)", EDIF_REF(wire->name)), wire->port_input));
				} else {
					int b[2];
					b[wire->upto ? 0 : 1] = wire->start_offset;
					b[wire->upto ? 1 : 0] = wire->start_offset + GetSize(wire) - 1;
					*f << stringf("          (port (array %s %d) (direction %s)", EDIF_DEFR(wire->name, port_rename, b[0], b[1]), wire->width, dir);
					if (attr_properties)
						for (auto &p : wire->attributes)
							add_prop(p.first, p.second);

					*f << ")\n";
					for (int i = 0; i < wire->width; i++) {
						RTLIL::SigSpec sig = sigmap(RTLIL::SigSpec(wire, i));
						net_join_db[sig].insert(make_pair(stringf("(portRef (member %s %d))", EDIF_REF(wire->name), lsbidx ? i : GetSize(wire)-i-1), wire->port_input));
					}
				}
			}

			*f << stringf("        )\n");
			*f << stringf("        (contents\n");

			if (!nogndvcc) {
				*f << stringf("          (instance GND (viewRef VIEW_NETLIST (cellRef GND (libraryRef LIB))))\n");
				*f << stringf("          (instance VCC (viewRef VIEW_NETLIST (cellRef VCC (libraryRef LIB))))\n");
			}

			for (auto cell : module->cells()) {
				*f << stringf("          (instance %s\n", EDIF_DEF(cell->name));
				*f << stringf("            (viewRef VIEW_NETLIST (cellRef %s%s))", EDIF_REF(cell->type),
						lib_cell_ports.count(cell->type) > 0 ? " (libraryRef LIB)" : "");
				for (auto &p : cell->parameters)
					add_prop(p.first, p.second);
				if (attr_properties)
					for (auto &p : cell->attributes)
						add_prop(p.first, p.second);

				*f << stringf(")\n");
				for (auto &p : cell->connections()) {
					RTLIL::SigSpec sig = sigmap(p.second);
					for (int i = 0; i < GetSize(sig); i++)
						if (sig[i].wire == NULL && sig[i] != RTLIL::State::S0 && sig[i] != RTLIL::State::S1)
							log_warning("Bit %d of cell port %s.%s.%s driven by %s will be left unconnected in EDIF output.\n",
									i, log_id(module), log_id(cell), log_id(p.first), log_signal(sig[i]));
						else {
							int member_idx = lsbidx ? i : GetSize(sig)-i-1;
							auto m = design->module(cell->type);
							int width = sig.size();
							if (m) {
								auto w = m->wire(p.first);
								if (w) {
									member_idx = lsbidx ? i : GetSize(w)-i-1;
									width = GetSize(w);
								}
							}
							if (width == 1)
								net_join_db[sig[i]].insert(make_pair(stringf("(portRef %s (instanceRef %s))", EDIF_REF(p.first), EDIF_REF(cell->name)), cell->output(p.first)));
							else {
								net_join_db[sig[i]].insert(make_pair(stringf("(portRef (member %s %d) (instanceRef %s))",
										EDIF_REF(p.first), member_idx, EDIF_REF(cell->name)), cell->output(p.first)));
							}
						}
				}
			}

			for (auto &it : net_join_db) {
				RTLIL::SigBit sig = it.first;
				if (sig.wire == NULL && sig != RTLIL::State::S0 && sig != RTLIL::State::S1) {
					if (sig == RTLIL::State::Sx) {
						for (auto &ref : it.second)
							log_warning("Exporting x-bit on %s as zero bit.\n", ref.first.c_str());
						sig = RTLIL::State::S0;
					} else if (sig == RTLIL::State::Sz) {
						continue;
					} else {
						for (auto &ref : it.second)
							log_error("Don't know how to handle %s on %s.\n", log_signal(sig), ref.first.c_str());
						log_abort();
					}
				}
				std::string netname;
				if (sig == RTLIL::State::S0)
					netname = "GND_NET";
				else if (sig == RTLIL::State::S1)
					netname = "VCC_NET";
				else {
					netname = log_signal(sig);
					for (size_t i = 0; i < netname.size(); i++)
						if (netname[i] == ' ' || netname[i] == '\\')
							netname.erase(netname.begin() + i--);
				}
				*f << stringf("          (net %s (joined\n", EDIF_DEF(netname));
				for (auto &ref : it.second)
					*f << stringf("              %s\n", ref.first.c_str());
				if (sig.wire == NULL) {
					if (nogndvcc)
						log_error("Design contains constant nodes (map with \"hilomap\" first).\n");
					if (sig == RTLIL::State::S0)
						*f << stringf("            (portRef %c (instanceRef GND))\n", gndvccy ? 'Y' : 'G');
					if (sig == RTLIL::State::S1)
						*f << stringf("            (portRef %c (instanceRef VCC))\n", gndvccy ? 'Y' : 'P');
				}
				*f << stringf("            )");
				if (attr_properties && sig.wire != NULL)
					for (auto &p : sig.wire->attributes)
						add_prop(p.first, p.second);
				*f << stringf("\n          )\n");
			}

			for (auto wire : module->wires())
			{
				if (!wire->get_bool_attribute(ID::keep))
					continue;

				for(int i = 0; i < wire->width; i++)
				{
					SigBit raw_sig = RTLIL::SigSpec(wire, i);
					SigBit mapped_sig = sigmap(raw_sig);

					if (raw_sig == mapped_sig || net_join_db.count(mapped_sig) == 0)
						continue;

					std::string netname = log_signal(raw_sig);
					for (size_t i = 0; i < netname.size(); i++)
						if (netname[i] == ' ' || netname[i] == '\\')
							netname.erase(netname.begin() + i--);

					if (keepmode)
					{
						*f << stringf("          (net %s (joined\n", EDIF_DEF(netname));

						auto &refs = net_join_db.at(mapped_sig);
						for (auto &ref : refs)
							if (ref.second)
								*f << stringf("              %s\n", ref.first.c_str());
						*f << stringf("            )");

						if (attr_properties && raw_sig.wire != NULL)
							for (auto &p : raw_sig.wire->attributes)
								add_prop(p.first, p.second);

						*f << stringf("\n          )\n");
					}
					else
					{
						log_warning("Ignoring conflicting 'keep' property on net %s. Use -keep to generate the extra net nevertheless.\n", EDIF_DEF(netname));
					}
				}
			}

			*f << stringf("        )\n");
			*f << stringf("      )\n");
			*f << stringf("    )\n");
		}
		*f << stringf("  )\n");

		*f << stringf("  (design %s\n", EDIF_DEF(top_module_name));
		*f << stringf("    (cellRef %s (libraryRef DESIGN))\n", EDIF_REF(top_module_name));
		*f << stringf("  )\n");

		*f << stringf(")\n");
	}
} EdifBackend;

PRIVATE_NAMESPACE_END
