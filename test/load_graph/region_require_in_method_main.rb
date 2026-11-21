# A require in a method body of main's loads its file where the method
# first runs: a call ahead of that raises NoMethodError, also in the
# statement that calls the method, before the call; the calls after it run
# the file's method, the call that loaded it answers true, a second false.
def err(e = $!) = e.is_a?(NameError) ? [e.class, e.name] : e.class
class RegionM; end
def load_it = require_relative("region_require_in_method_lib")
p [(RegionM.new.m rescue err), load_it, RegionM.new.m]
p load_it
p RegionM.new.m(1)
