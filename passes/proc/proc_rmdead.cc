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

#include "kernel/register.h"
#include "kernel/bitpattern.h"
#include "kernel/log.h"
#include <sstream>
#include <stdlib.h>
#include <stdio.h>
#include <set>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

static bool can_use_fully_defined_pool(RTLIL::SwitchRule *sw)
{
	if (!GetSize(sw->signal))
		return false;

	for (const RTLIL::SigBit &bit : sw->signal)
		if (bit.wire == NULL)
			return false;

	for (const RTLIL::CaseRule *cas : sw->cases)
		for (const RTLIL::SigSpec &sig : cas->compare)
			if (!sig.is_fully_def())
				return false;

	return true;
}

// This replicates the necessary parts of BitPatternPool's interface, but rather
// than storing remaining patterns, this explicitly stores which fully-defined
// constants have already been matched.
struct FullyDefinedPool
{
	FullyDefinedPool(const RTLIL::SigSpec &signal)
		: max_patterns{signal.size() >= 32 ? 0ul : 1ul << signal.size()}
	{}

	bool take(RTLIL::SigSpec sig)
	{
		if (default_reached || patterns.count(sig))
			return false;
		patterns.insert(sig);
		return true;
	}

	void take_all()
	{
		default_reached = true;
	}

	bool empty()
	{
		return default_reached ||
			(max_patterns && max_patterns == patterns.size());
	}

	pool<RTLIL::SigSpec> patterns;
	bool default_reached = false;
	size_t max_patterns;
};

void proc_rmdead(RTLIL::SwitchRule *sw, int &counter, int &full_case_counter);

template <class Pool>
static void proc_rmdead_impl(RTLIL::SwitchRule *sw, int &counter, int &full_case_counter)
{
	Pool pool(sw->signal);

	for (size_t i = 0; i < sw->cases.size(); i++)
	{
		bool is_default = GetSize(sw->cases[i]->compare) == 0 && (!pool.empty() || GetSize(sw->signal) == 0);

		for (size_t j = 0; j < sw->cases[i]->compare.size(); j++) {
			RTLIL::SigSpec sig = sw->cases[i]->compare[j];
			if (!sig.is_fully_const())
				continue;
			if (!pool.take(sig))
				sw->cases[i]->compare.erase(sw->cases[i]->compare.begin() + (j--));
		}

		if (!is_default) {
			if (sw->cases[i]->compare.size() == 0) {
				delete sw->cases[i];
				sw->cases.erase(sw->cases.begin() + (i--));
				counter++;
				continue;
			}
			// if (pool.empty())
			// 	sw->cases[i]->compare.clear();
		}

		for (auto switch_it : sw->cases[i]->switches)
			proc_rmdead(switch_it, counter, full_case_counter);

		if (is_default)
			pool.take_all();
	}

	if (pool.empty() && !sw->get_bool_attribute(ID::full_case)) {
		sw->set_bool_attribute(ID::full_case);
		full_case_counter++;
	}
}

void proc_rmdead(RTLIL::SwitchRule *sw, int &counter, int &full_case_counter)
{
	if (can_use_fully_defined_pool(sw))
		proc_rmdead_impl<FullyDefinedPool>(sw, counter, full_case_counter);
	else
		proc_rmdead_impl<BitPatternPool>(sw, counter, full_case_counter);
}

struct ProcRmdeadPass : public Pass {
	ProcRmdeadPass() : Pass("proc_rmdead", "eliminate dead trees in decision trees") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    proc_rmdead [selection]\n");
		log("\n");
		log("This pass identifies unreachable branches in decision trees and removes them.\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		log_header(design, "Executing PROC_RMDEAD pass (remove dead branches from decision trees).\n");

		extra_args(args, 1, design);

		int total_counter = 0;
		for (auto mod : design->all_selected_modules()) {
			for (auto proc : mod->selected_processes()) {
				int counter = 0, full_case_counter = 0;
				for (auto switch_it : proc->root_case.switches)
					proc_rmdead(switch_it, counter, full_case_counter);
				if (counter > 0)
					log("Removed %d dead cases from process %s in module %s.\n", counter,
							log_id(proc), log_id(mod));
				if (full_case_counter > 0)
					log("Marked %d switch rules as full_case in process %s in module %s.\n",
							full_case_counter, log_id(proc), log_id(mod));
				total_counter += counter;
			}
		}

		log("Removed a total of %d dead cases.\n", total_counter);
	}
} ProcRmdeadPass;

PRIVATE_NAMESPACE_END
