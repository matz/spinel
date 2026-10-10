# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# -2**63 crosses a method parameter, a return, a block parameter and a proc,
# in the plain and in the nullable form (the nullable ones also see nil).
def plain(v) = v
def ret_plain(n) = -9223372036854775807 - (n + 1)
def opt(v) = v
def ret_opt(n)
  return nil if n > 100
  -9223372036854775807 - (n + 1)
end
n = ARGV.size
m = ret_plain(n)
p plain(m)
p ret_plain(n)
p opt(nil)
p opt(m)
p ret_opt(1000)
p ret_opt(n)
r = ret_opt(n)
p r.nil?, r ? 1 : 2, r
q = opt(m)
p q.nil?, q.zero?, "#{q}"
[m].each { |e| p e; p e.nil? }
[m, nil].each { |e| p e; p e.nil? }
pr = proc { |v| v }
p pr.call(m)
l = ->(v) { v.nil? ? "nil" : v.to_s }
puts l.call(m)
puts l.call(nil)
def yielder(n)
  yield(-9223372036854775807 - (n + 1))
end
yielder(n) { |v| p v; p v.nil? }
