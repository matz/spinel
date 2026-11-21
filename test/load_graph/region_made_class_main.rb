# A class or module a required file makes is reached only once the file has
# run, so its methods there are there whenever a call reaches them (the C
# calls them directly); a module it prepends to the entry's class changes
# that class's method from the require on. So too for a file a method loads,
# which no load point places: its own classes, nested ones too, answer
# directly, from inside a block and boxed, while the entry's class it reopens
# has the method only from the call on.
def err(e = $!) = e.is_a?(NameError) ? [e.class, e.name] : e.class
class RegionFoo; def hi = "foo"; end
p RegionFoo.new.hi
require_relative "region_made_class_lib"
p RegionMade.new.m, RegionMade::In.new.i, RegionMade.k, RegionFoo.new.hi
class RegionMade::In; def j = :j; end
p RegionMade::In.new.j
def load_more = require_relative("region_made_class_more")
p((RegionFoo.new.more rescue err))
load_more
p RegionMore.new.v, RegionMore::Deep.new.d, RegionMore.k, RegionFoo.new.more
p [RegionMore.new, nil].map { |o| o&.v }
