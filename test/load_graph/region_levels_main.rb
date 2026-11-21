# A require in a method of a file that a method of the entry requires, three
# files deep: each level's methods are there from the call that loads it.
def err(e = $!) = e.is_a?(NameError) ? [e.class, e.name] : e.class
class RegionL; end
def lvl1 = require_relative("region_levels_b")
p((RegionL.new.m rescue err))
lvl1
p RegionL.new.m
