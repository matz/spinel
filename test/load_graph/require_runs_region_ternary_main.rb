# A require as a ternary's branch, in an argument: the required file's top
# level runs inside that statement, so it sees K's first v, not the second
class K; def v = 1; end
def probe = K.new.v
p(ARGV.size == 0 ? require_relative("require_runs_region_ternary_lib") : nil)
class K; def v = 2; end
puts probe
