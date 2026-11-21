# A method a required file defines on a class the entry made is not there
# before the require runs: a call ahead of it raises NoMethodError, on any
# receiver (a nil one by `&.`, one boxed in an Array), and the same calls
# after it run the file's method.
def err(e = $!) = e.is_a?(NameError) ? [e.class, e.name] : e.class
class RegionK; end
x = ARGV.empty? ? RegionK.new : nil
p((x&.m rescue err))
os = [RegionK.new, 1, nil]
os.each { |o| p((o.m rescue err)) }
p require_relative("region_call_before_require_lib")
p x&.m
os.each { |o| p((o.m rescue err)) }
