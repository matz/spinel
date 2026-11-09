# Symbol#to_s and #id2name answer a new String at each call, as CRuby does:
# two calls answered one object (the runtime's one frozen copy of the name,
# which it keeps for puts, interpolation and inspect). Frozen or not is
# Spinel's own answer (docs/limitations.md; test/symbol_to_s_frozen.rb).
s = :abc
a = s.to_s
p a.equal?(s.to_s), s.id2name.equal?(s.id2name), a.equal?(a), a == s.to_s

# a boxed Symbol's to_s
v = [:def, 1][0]
x = v.to_s
p v.to_s.equal?(v.to_s), x.equal?(x), x

# String(sym) calls to_s
p String(:ghi).equal?(String(:ghi))

# a copy changed leaves the name alone
d = s.to_s.dup
d << "!"
p d, s.to_s

# name stays the one frozen String (CRuby's interned name)
p s.name.frozen?, s.name.equal?(s.name)
puts "#{s} #{v}"
