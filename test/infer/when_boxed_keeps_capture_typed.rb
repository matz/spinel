# A boxed `when` operand widens only the Method captures whose value can
# be boxed: sq, held in a Method local and only called, keeps its typed
# parameter (infer-test greps its signature).
def sq(x) = x * x
m = method(:sq)
v = [1, "a"][ARGV.size]
pat = [1, "a"][0]
case v
when pat then puts "one"
else puts "other"
end
s = 0
3_000_000.times { |i| s += m.call(i & 1023) }
p s
