# A nil block value in an Integer or Float sum { } is the accumulator's
# coercion failure, as CRuby's Array#sum: "nil can't be coerced into"
# Integer while the running sum is still the Integer it started at (the
# first term, or every term of an Integer sum), Float once a Float term has
# been added or the start is a Float.
def t
  yield
rescue => e
  puts "#{e.class}: #{e.message}"
end
n = ARGV.size
t { p [1, 2, [3][n + 5]].sum { |e| e } }
t { p [[1][n + 5], 2].sum { |e| e } }
t { p [1, 2].sum(10) { |e| [e][n + 5] } }
t { p [1.5, 2.5].sum(1) { |e| e > 2 ? [e][n + 5] : e } }
t { p [1.5, 2.5].sum(1) { |e| e < 2 ? [e][n + 5] : e } }
t { p [1, 2].sum(0.5) { |e| e > 1 ? [e][n + 5] : e } }
t { p [1.5, 2.5].sum(0.0) { |e| [e][n + 5] } }
t { p [1, 2].sum { |e| next [e][n + 5] if e > 1; e } }
t { p [1.5, 2.5].sum { |e| next [e][n + 5] if e > 2; e } }
p [1, 2].sum { |e| e * 2 }
p [1.5, 2.5].sum { |e| e }
p [1, 2].sum(0.5) { |e| e }
