# A file only a method nothing calls, or a lambda nothing calls, would load
# never runs: its methods are never there.
def err(e = $!) = e.is_a?(NameError) ? [e.class, e.name] : e.class
class RegionN; end
def never_called = require_relative("region_never_loaded_a")
l = -> { require_relative "region_never_loaded_b" }
p((RegionN.new.never_a rescue err))
p((RegionN.new.never_b rescue err))
p RegionN.new.respond_to?(:never_a), RegionN.method_defined?(:never_b)
