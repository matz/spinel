# An OpenStruct given its members by String keys interns their Symbols at run
# time, and a Hash built from it renders those keys by name: {a: 2}, not
# {"": 2}. This program spells no Symbol, passes no keyword argument and
# aliases nothing on purpose: a Symbol name the compiler interns for the
# source already made the names render.
require "ostruct"

o = OpenStruct.new
o["a"] = 2
o["name"] = "x"
p o
p o.to_h
p o.to_h.keys
puts o.to_h.inspect
puts "#{o.to_h}"
p [o.to_h, 1]
p({"outer" => o.to_h})
k = o.to_h.keys.first
p k.to_s, k.length

p OpenStruct.new({"b" => 1}).to_h
h = {"k" => "v", "n" => [1, 2]}
q = OpenStruct.new(h)
p q.to_h
p q["k"], q["n"]

# Symbols made at run time order and match by name.
r = OpenStruct.new
r["zeta"] = 1
r["alpha"] = 2
r["mid"] = 3
ks = r.to_h.keys
p ks.sort, ks.max
p(/lph/ === ks[1])
case ks[2]
when /^m/ then p "by name"
else p "no match"
end
begin
  throw ks[0]
rescue UncaughtThrowError => e
  p e.message
end
