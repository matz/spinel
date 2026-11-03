# spinel: reject-subclass: a method defined under a condition only the run time decides is not answered for
# A lone def under a run-time condition at the top level is Object's, and so
# every class's: a list of any class's methods stays refused.
$on = ENV["SPINEL_TEST_UNSET_VARIABLE"].nil?
def mm = 1 if !$on
class Other; end
p Other.new.methods.include?(:mm)
