# The first-run analysis over several regions (--dump-load-record,
# record_region_runs_main.rb.load-record): a file no load places is a
# region of its own, scanned at the first statement of the entry that may
# load it, as code that statement runs. Region 1 loads from load_a's body,
# at its first run (#6); region 3 from a top-level conditional ahead of
# its second require (#8); region 4 from a block that runs at once (#10);
# region 2 never, since nothing calls load_d. probe's body first runs
# where region 1 calls it, and so does each K#v it may reach.
class K; def v = 1; end
def probe = K.new.v
def load_a = require_relative("record_region_runs_a")
def load_d = require_relative("record_region_runs_d")
load_a
require_relative "record_region_runs_b" if ARGV.size == 0
[1].each { require_relative "record_region_runs_c" }
require_relative "record_region_runs_b"
require_relative "record_region_runs_c"
class K; def v = 2; end
puts probe
