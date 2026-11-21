# A require in a value (a ternary's branch, an Array literal's element, a
# method's tail) runs where that value is made: the calls before it raise
# NoMethodError, those after it run the file's method.
def err(e = $!) = e.is_a?(NameError) ? [e.class, e.name] : e.class
class RegionV; end
p((RegionV.new.t rescue err))
r = ARGV.empty? ? require_relative("region_value_position_a") : nil
p r, RegionV.new.t
a = [(RegionV.new.u rescue err), require_relative("region_value_position_b"), RegionV.new.u]
p a
def tail_load = require_relative("region_value_position_c")
p((RegionV.new.w rescue err))
p tail_load, RegionV.new.w
