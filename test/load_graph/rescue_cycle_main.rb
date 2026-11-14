# Two files that require each other, the first then requiring a missing
# one: loading either raises LoadError. The rescued body in main.rb asks
# the first file ahead of the third file's rescued body, which requires the
# second; the second's answer is the cycle's, not read while the first is
# still being walked. The def after that require did not run.
def rescue_cycle_f = :old
require_relative "rescue_cycle_c_lib"
begin
  require_relative "rescue_cycle_a_lib"
rescue LoadError
  p :a_failed
end
p rescue_cycle_f
