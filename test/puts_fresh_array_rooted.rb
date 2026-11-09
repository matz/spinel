# A fresh Array that only puts holds -- a method's answer handed straight to
# it -- stays rooted while its elements render, which allocates: with a
# collection between elements (SPINEL_GC_STRESS) it was freed partway, and
# the rest of its lines came out empty or not at all. Bare puts of an Array
# of mixed classes (and of a table under --int-overflow=promote, where it
# stays boxed), IO#puts of any Array, and warn of one.
# spinel: gc-stress

def floats = Array.new(12) { |i| [i * 1.5, i / 4.0] }
def mixed = Array.new(12) { |i| i.even? ? i * 0.5 : "s#{i}" }
def words = Array.new(8) { |i| "w#{i}" * 2 }

puts mixed
puts floats
puts floats, mixed
$stdout.puts floats
STDOUT.puts mixed
STDOUT.puts words, floats
# warn renders an Array a line per element too; $stderr is $stdout here so
# the lines are compared
$stderr = $stdout
warn mixed
warn floats, words
puts "end"
