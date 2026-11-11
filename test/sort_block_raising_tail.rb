# A sort, sort!, min, max or minmax block whose last expression raises (a
# `raise`, a call of a method the element does not have, `&:name` of such a
# method) runs and raises, as in CRuby. The blockless sort ran in its place
# and answered as if no block had been given.
def t
  yield
rescue NoMethodError => e
  puts "NoMethodError #{e.name}"
rescue => e
  puts "#{e.class} #{e.message}"
end

t { p [3, 1].sort(&:nil_nope) }
t { a = [3, 1]; a.sort!(&:nil_nope); p a }
t { p [3, 1].sort { |a, b| a.nil_nope(b) } }
t { p [3, 1].sort { |a, b| raise "sort" } }
t { x = [3, 1]; x.sort! { |a, b| raise "sort!" }; p x }
t { p %w[b a].sort { |a, b| a.zork(b) } }
t { p [1.5, 0.5].sort { |a, b| a.zork(b) } }
t { p [[1], [0]].sort { |a, b| a.zork(b) } }
t { p [1, "a"].sort { |a, b| a.zork(b) } }
t { p({ a: 1, b: 2 }.sort { |a, b| a.zork(b) }) }
t { p (1..3).sort { |a, b| a.zork(b) } }
t { p [3, 1].max { |a, b| a.nil_nope(b) } }
t { p [3, 1].min { |a, b| raise "min" } }
t { p [3, 1].max(&:nil_nope) }
t { p [3, 1].min(&:nil_nope) }
t { p [3, 1].minmax(&:nil_nope) }
t { p [3, 1, 2].max(2) { |a, b| a.zork(b) } }
t { p (1..3).max { |a, b| a.zork(b) } }
t { p [3, 1].sort { |a, b| throw :x } }

def in_method(x) = x.sort { |a, b| a.zork(b) }
t { p in_method([3, 1]) }

# one element or none: the block is never called
t { p [3].sort { |a, b| raise "never" } }
t { p [].max { |a, b| raise "never" } }

# blocks that answer stay as they were
t { p [5, 1, 3].sort { |a, b| b <=> a } }
t { p [3, 1].sort { |a, b| raise "big" if a > 5; b <=> a } }
t { p %w[b a].sort(&:casecmp) }
t { p [3, 1].sort(&:<=>) }
