def pass(x)
  yield x
end

s = (S = +"ab\0cd")
GC.start
s << "!"
p [S.bytes, s.equal?(S)]

a = (A = (B = +"nest"))
GC.start
a << "?"
p [A, B, a.equal?(A), A.equal?(B)]

v = pass(0) { TAIL = +"tail" }
GC.start
v << "!"
p [TAIL, v.equal?(TAIL)]

poly = [7, (BOXED = +"box")][1]
poly << "!"
p [BOXED, poly.equal?(BOXED)]

frozen = (FROZEN = "lit")
p [frozen.equal?(FROZEN), frozen.frozen?]

p((DIRECT = +"direct").concat("!").equal?(DIRECT))
