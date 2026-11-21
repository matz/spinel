# A require that may not have run leaves the calls after it to ask whether
# its file has: a method called only under a condition, or in a block, or
# made twice, loads the file when it runs, and the calls before that raise
# NoMethodError.
def err(e = $!) = e.is_a?(NameError) ? [e.class, e.name] : e.class
class RegionS; end
def setup = require_relative("region_not_sure_lib")
p((RegionS.new.m rescue err))
[1].each { setup } if ARGV.size > 5
p((RegionS.new.m rescue err))
2.times { setup } unless ARGV.size > 5
p RegionS.new.m
