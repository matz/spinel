# A fresh Array that only puts holds -- a method's answer handed straight to
# it -- stays rooted while its elements render, which allocates: with a
# collection between elements (SPINEL_GC_STRESS) it was freed partway, and
# the rest of its lines came out empty or not at all. Bare puts of an Array
# of mixed classes (and of a table under --int-overflow=promote, where it
# stays boxed), puts of a method's answer of more than one class that is
# an Array, IO#puts of any Array, and warn of one.
# spinel: gc-stress

def floats = Array.new(12) { |i| [i * 1.5, i / 4.0] }
def mixed = Array.new(12) { |i| i.even? ? i * 0.5 : "s#{i}" }
def words = Array.new(8) { |i| "w#{i}" * 2 }
# an answer of more than one class, an Array or not
def pick(i) = i.even? ? Array.new(12) { |k| [k * 1.5, "r#{k}"] } : "odd"
def pick2(i) = i.even? ? Array.new(12) { |k| k * 0.25 } : :sym

puts mixed
puts floats
puts floats, mixed
$stdout.puts floats
STDOUT.puts mixed
STDOUT.puts words, floats
puts pick(0)
puts pick2(0), pick(1), pick2(1)
$stdout.puts pick(0)
# warn renders an Array a line per element too; $stderr is $stdout here so
# the lines are compared
$stderr = $stdout
warn mixed
warn floats, words
puts "end"
