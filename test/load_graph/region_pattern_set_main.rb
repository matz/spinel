# A require whose path the run time completes (`"lib_#{name}"`) may load any
# file of the program its literal head and tail match. In a block that surely
# runs it loads one of them, not each: a method of the file it did not load
# stays undefined, also in the statement after the block and boxed.
def err(e = $!) = e.is_a?(NameError) ? [e.class, e.name] : e.class
class RegionPat; end
def never_called = [require_relative("region_pattern_lib_a"), require_relative("region_pattern_lib_b")]
name = ARGV.empty? ? "b" : "a"
1.times { require_relative "region_pattern_lib_#{name}" }
p((RegionPat.new.pa rescue err))
p RegionPat.new.pb
p [RegionPat.new, nil].map { |o| (o&.pa rescue err) }
