# Enumerable's cycle, min_by, max_by and tally take an optional argument
# that is absent only when it is nil. false was taken for no argument:
# `cycle(false) { }` looped forever, `min_by(false)` answered one element and
# `tally(false)` a fresh Hash, where CRuby raises TypeError.

def t
  r = yield
  r.inspect
rescue => e
  "#{e.class}: #{e.message}"
end

p t { [3, 1].cycle(false) { } }
p t { (1..2).cycle(false) { } }
p t { { a: 1 }.cycle(false) { } }
p t { [3, 1].min_by(false) { |x| x } }
p t { [3, 1].max_by(false) { |x| x } }
p t { [3, 1].each_entry.max_by(false) { |x| x } }
p t { (1..3).min_by(false) { |x| -x } }
p t { [3, 1].tally(false) }

# nil, and arguments of the right class, work as before
out = []
[3, 1].cycle(2) { |x| out << x }
p out
out = []
[3, 1].cycle(nil) { |x| out << x; break if out.size == 5 }
p out
p [3, 1, 2].min_by(nil) { |x| x }, [3, 1, 2].max_by(2) { |x| x }
h = { 3 => 1 }
p [3, 1].tally(h), h
