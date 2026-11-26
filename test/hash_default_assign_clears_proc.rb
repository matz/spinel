# Hash#default= clears the default proc: a missing key then reads the
# assigned default, and default_proc is nil.
h = Hash.new { :blk }
h[:a] = 1
h[:b] = "s"
h.default = :plain
p h[:zz], h.default_proc.nil?, h.default

g = Hash.new { |hash, k| k.to_s * 2 }
g["x"] = 1.5
g["y"] = :sym
g.default = nil
p g["zz"], g.default_proc.nil?

n = Hash.new { 9 }
n[:a] = "s"
n[:b] = 2
n.default = 4
p n[:zz], n.default_proc.nil?

# through a boxed receiver
b = Hash.new { :blk }
b[:a] = 1
b[:b] = "s"
x = [b, 1][0]
x.default = :plain
p b[:zz], b.default_proc.nil?

# a later default_proc= takes over again
h.default_proc = proc { :again }
p h[:zz], h.default

# a default proc read before default= keeps running its block
s = Hash.new { |hh, k| [:blk, k, hh.size] }
s[:a] = 1
s[:b] = "s"
saved = s.default_proc
s.default = :plain
p saved.call(s, :zz), s[:zz]
q = Hash.new { |hh, k| k.to_s }
q[1] = :x
q["s"] = 2
sv = q.default_proc
q.default = 0
p sv.call(q, 5), q[:q]
