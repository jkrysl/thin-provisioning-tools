#include <iostream>
#include <sstream>
#include <string>
#include <stdexcept>

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "base/error_state.h"
#include "base/error_string.h"
#include "base/file_utils.h"
#include "base/nested_output.h"
#include "caching/commands.h"
#include "caching/metadata.h"
#include "persistent-data/block.h"
#include "persistent-data/file_utils.h"
#include "persistent-data/space_map.h"
#include "persistent-data/space-maps/core.h"
#include "version.h"

using namespace boost;
using namespace caching;
using namespace persistent_data;
using namespace std;

//----------------------------------------------------------------

namespace {
	class reporter_base {
	public:
		reporter_base(nested_output &o)
		: out_(o),
		  err_(NO_ERROR) {
		}

		virtual ~reporter_base() {}

		nested_output &out() {
			return out_;
		}

		nested_output::nest push() {
			return out_.push();
		}

		base::error_state get_error() const {
			return err_;
		}

		void mplus_error(error_state err) {
			err_ = combine_errors(err_, err);
		}

	private:
		nested_output &out_;
		error_state err_;
	};

	class superblock_reporter : public superblock_damage::damage_visitor, reporter_base {
	public:
		superblock_reporter(nested_output &o)
		: reporter_base(o) {
		}

		virtual void visit(superblock_damage::superblock_corrupt const &d) {
			out() << "superblock is corrupt" << end_message();
			{
				nested_output::nest _ = push();
				out() << d.get_desc() << end_message();
			}

			mplus_error(FATAL);
		}

		virtual void visit(superblock_damage::superblock_invalid const &d) {
			out() << "superblock is invalid" << end_message();
			{
				nested_output::nest _ = push();
				out() << d.get_desc() << end_message();
			}

			mplus_error(FATAL);
		}

		using reporter_base::get_error;
	};

	class mapping_reporter : public mapping_array_damage::damage_visitor, reporter_base {
	public:
		mapping_reporter(nested_output &o)
		: reporter_base(o) {
		}

		virtual void visit(mapping_array_damage::missing_mappings const &d) {
			out() << "missing mappings " << d.keys_ << ":" << end_message();
			{
				nested_output::nest _ = push();
				out() << d.get_desc() << end_message();
			}

			mplus_error(FATAL);
		}

		virtual void visit(mapping_array_damage::invalid_mapping const &d) {
			out() << "invalid mapping:" << end_message();
			{
				nested_output::nest _ = push();
				out() << d.get_desc()
				      << " [cblock = " << d.cblock_
				      << ", oblock = " << d.m_.oblock_
				      << ", flags = " << d.m_.flags_
				      << "]" << end_message();
			}

			mplus_error(FATAL);
		}

		using reporter_base::get_error;
	};

	class hint_reporter : public hint_array_damage::damage_visitor, reporter_base {
	public:
		hint_reporter(nested_output &o)
		: reporter_base(o) {
		}

		virtual void visit(hint_array_damage::missing_hints const &d) {
			out() << "missing mappings " << d.keys_ << ":" << end_message();
			{
				nested_output::nest _ = push();
				out() << d.get_desc() << end_message();
			}

			mplus_error(FATAL);
		}

		using reporter_base::get_error;
	};

	class discard_reporter : public bitset_detail::bitset_visitor, reporter_base {
	public:
		discard_reporter(nested_output &o)
		: reporter_base(o) {
		}

		virtual void visit(uint32_t index, bool value) {
			// no op
		}

		virtual void visit(bitset_detail::missing_bits const &d) {
			out() << "missing discard bits "  << d.keys_ << end_message();
			mplus_error(FATAL);
		}

		using reporter_base::get_error;
	};

	class space_map_reporter : public space_map_detail::visitor, reporter_base {
	public:
		space_map_reporter(nested_output &o)
		: reporter_base(o) {
		}

		using reporter_base::get_error;
	};

	//--------------------------------

	struct flags {
		flags()
			: check_mappings_(true),
			  check_hints_(true),
			  check_discards_(true),
			  ignore_non_fatal_errors_(false),
			  quiet_(false),
			  clear_needs_check_on_success_(false) {
		}

		bool check_mappings_;
		bool check_hints_;
		bool check_discards_;
		bool ignore_non_fatal_errors_;
		bool quiet_;
		bool clear_needs_check_on_success_;
	};

	struct stat guarded_stat(string const &path) {
		struct stat info;

		int r = ::stat(path.c_str(), &info);
		if (r) {
			ostringstream msg;
			msg << path << ": " << error_string(errno);
			throw runtime_error(msg.str());
		}

		return info;
	}

	void clear_needs_check(string const &path) {
		block_manager<>::ptr bm = open_bm(path, block_manager<>::READ_WRITE);

		superblock sb = read_superblock(bm);
		sb.flags.clear_flag(superblock_flags::NEEDS_CHECK);
		write_superblock(bm, sb);
	}

	error_state metadata_check(string const &path, flags const &fs,
				   bool &needs_check_set) {
		nested_output out(cerr, 2);
		if (fs.quiet_)
			out.disable();

		if (file_utils::get_file_length(path) < persistent_data::MD_BLOCK_SIZE) {
			if (check_for_xml(bm)) {
			    out << "This looks like XML.  cache_check only checks the binary metadata format." << end_message();
			    return FATAL;
			}
			out << "Metadata device/file too small.  Is this binary metadata?"
			    << end_message();
			return FATAL;
		}

		block_manager<>::ptr bm = open_bm(path, block_manager<>::READ_ONLY);

		superblock_reporter sb_rep(out);
		mapping_reporter mapping_rep(out);
		hint_reporter hint_rep(out);
		discard_reporter discard_rep(out);

		out << "examining superblock" << end_message();
		{
			nested_output::nest _ = out.push();
			check_superblock(bm, bm->get_nr_blocks(), sb_rep);
		}

		if (sb_rep.get_error() == FATAL)
			return FATAL;

		superblock sb = read_superblock(bm);
		transaction_manager::ptr tm = open_tm(bm, SUPERBLOCK_LOCATION);

		needs_check_set = sb.flags.get_flag(superblock_flags::NEEDS_CHECK);

		if (fs.check_mappings_) {
			out << "examining mapping array" << end_message();
			{
				nested_output::nest _ = out.push();
				mapping_array ma(*tm, mapping_array::ref_counter(), sb.mapping_root, sb.cache_blocks);
				check_mapping_array(ma, mapping_rep, sb.version);
			}

			if (sb.version >= 2) {
				persistent_data::bitset dirty(*tm, *sb.dirty_root, sb.cache_blocks);
				// FIXME: is there no bitset checker?
			}
		}

		if (fs.check_hints_) {
			if (!sb.hint_root)
				out << "no hint array present" << end_message();

			else {
				out << "examining hint array" << end_message();
				{
					nested_output::nest _ = out.push();
					hint_array ha(*tm, sb.policy_hint_size, sb.hint_root, sb.cache_blocks);
					ha.check(hint_rep);
				}
			}
		}

		if (fs.check_discards_) {
			if (!sb.discard_root)
				out << "no discard bitset present" << end_message();

			else {
				out << "examining discard bitset" << end_message();
				{
					nested_output::nest _ = out.push();
					persistent_data::bitset discards(*tm, sb.discard_root, sb.discard_nr_blocks);
					// FIXME: is there no bitset checker?
				}
			}
		}

		// FIXME: make an error class that's an instance of mplus
		return combine_errors(sb_rep.get_error(),
				      combine_errors(mapping_rep.get_error(),
						     combine_errors(hint_rep.get_error(),
								    discard_rep.get_error())));
	}

	int check(string const &path, flags const &fs) {
		error_state err;
		bool needs_check_set;
		struct stat info = guarded_stat(path);

		if (!S_ISREG(info.st_mode) && !S_ISBLK(info.st_mode)) {
			ostringstream msg;
			msg << path << ": " << "Not a block device or regular file";
			throw runtime_error(msg.str());
		}

		err = metadata_check(path, fs, needs_check_set);

		bool success = false;

		if (fs.ignore_non_fatal_errors_)
			success = (err == FATAL) ? false : true;
		else
			success = (err == NO_ERROR) ? true : false;

		if (success && fs.clear_needs_check_on_success_ && needs_check_set)
			clear_needs_check(path);

		return err == NO_ERROR ? 0 : 1;
	}

	int check_with_exception_handling(string const &path, flags const &fs) {
		int r;
		try {
			r = check(path, fs);

		} catch (std::exception &e) {
			if (!fs.quiet_)
				cerr << e.what() << endl;
			r = 1;
		}

		return r;

	}
}

//----------------------------------------------------------------

cache_check_cmd::cache_check_cmd()
	: command("cache_check")
{
}

void
cache_check_cmd::usage(std::ostream &out) const
{
	out << "Usage: " << get_name() << " [options] {device|file}" << endl
	    << "Options:" << endl
	    << "  {-q|--quiet}" << endl
	    << "  {-h|--help}" << endl
	    << "  {-V|--version}" << endl
	    << "  {--clear-needs-check-flag}" << endl
	    << "  {--super-block-only}" << endl
	    << "  {--skip-mappings}" << endl
	    << "  {--skip-hints}" << endl
	    << "  {--skip-discards}" << endl;
}

int
cache_check_cmd::run(int argc, char **argv)
{
	int c;
	flags fs;
	const char shortopts[] = "qhV";
	const struct option longopts[] = {
		{ "quiet", no_argument, NULL, 'q' },
		{ "super-block-only", no_argument, NULL, 1 },
		{ "skip-mappings", no_argument, NULL, 2 },
		{ "skip-hints", no_argument, NULL, 3 },
		{ "skip-discards", no_argument, NULL, 4 },
		{ "clear-needs-check-flag", no_argument, NULL, 5 },
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ NULL, no_argument, NULL, 0 }
	};

	while ((c = getopt_long(argc, argv, shortopts, longopts, NULL)) != -1) {
		switch(c) {
		case 1:
			fs.check_mappings_ = false;
			fs.check_hints_ = false;
			break;

		case 2:
			fs.check_mappings_ = false;
			break;

		case 3:
			fs.check_hints_ = false;
			break;

		case 4:
			fs.check_discards_ = false;
			break;

		case 5:
			fs.clear_needs_check_on_success_ = true;
			break;

		case 'h':
			usage(cout);
			return 0;

		case 'q':
			fs.quiet_ = true;
			break;

		case 'V':
			cout << THIN_PROVISIONING_TOOLS_VERSION << endl;
			return 0;

		default:
			usage(cerr);
			return 1;
		}
	}

	if (argc == optind) {
		cerr << "No input file provided." << endl;
		usage(cerr);
		return 1;
	}

	return check_with_exception_handling(argv[optind], fs);
}

//----------------------------------------------------------------
