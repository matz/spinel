# A file required under a condition first, then at the top level: its top
# level runs at the first require only, before K's second v
class K; def v = 1; end
def probe = K.new.v
require_relative "require_runs_region_twice_lib" if ARGV.size == 0
class K; def v = 2; end
puts probe
require_relative "require_runs_region_twice_lib"
