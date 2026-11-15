# String#[]=, insert and slice! change the String where it lives: a loop of
# them over a long String costs what it moves, and every name holding the
# String sees each change. The errors, their order, the encoding the result
# takes and what #length and #[] read afterwards are CRuby's.
# spinel: gc-stress

def show(s) = [s, s.length, s.bytesize, s.encoding.to_s, s.ascii_only?]

def err
  yield
  :none
rescue => e
  [e.class, e.message]
end

# each []= form, on a 7-bit String
s = +"abcdef"
a = s
s[0] = "X"
s[-1] = "YZ"
s[s.length] = "!"
s[1, 2] = ""
s[2..3] = "mn"
s[..0] = "<"
s[5..] = ">"
s[/m(n)/, 1] = "N"
s[/<</] = "" if s.include?("<<")
s["N"] = "nn"
p show(s), a.equal?(s), s[2], s[-1]

# out of range, a negative length, a missing key, each before FrozenError
p err { s[99] = "q" }, err { s[1, -1] = "q" }, err { s[99..] = "q" }, err { s["zz"] = "q" }
f = "frozen".freeze
p err { f[99] = "q" }, err { f[0] = "q" }, err { f.insert(99, "q") }, err { f.slice!(0) }

# characters, not bytes: a 7-bit String takes a multibyte character, and
# indexing past it reads characters
u = +"hello"
u[1] = "é"
p show(u), u[2], u[-1], u.index("l")
u[0, 2] = "ab"
p show(u), u[1], u.length
u.insert(2, "日本")
p show(u), u[3], u[4]
u.insert(-1, "!")
u.insert(-3, "?")
p show(u)
p u.slice!(2), u.slice!(2, 2), u.slice!(1..), u, u.length, u.encoding.to_s

# a String with an invalid byte, measured, then spliced: #length and #[]
# read what its bytes are now, valid text or still invalid
iv = +"\xff\u00e9!"
iv.length
iv[0] = "a"
p iv, iv.length, iv[1], iv.valid_encoding?
jn = +"\xC3a\xA9"
jn.length
jn.slice!(1)
p jn, jn.length, jn[0]
kp = +"\xffab"
kp.length
kp[2] = "c"
kp.insert(1, "z")
p kp.length, kp[2], kp.valid_encoding?

# a binary String with a UTF-8 value: ASCII-only, it takes UTF-8; given
# 7-bit text it stays binary
b = "ab".b
b[0] = "é"
p show(b)
c = "\xff\xfe".b
c[0] = "z"
p show(c), c.bytes
d = "\xff\xfe".b
d.insert(1, "q")
p show(d), d.bytes

# embedded NUL bytes are bytes like any other
z = +"a\0b"
z[1] = "\0\0"
z.insert(0, "\0")
z[-1, 1] = "c\0"
p z.bytes, z.length, z.slice!(1, 2).bytes, z.bytes

# the value is the String itself, or part of it
w = +"abc"
w[1] = w
p show(w)
w.insert(2, w)
p show(w)
w[0, 3] = w[3, 4]
p show(w)

# Strings made from the String's bytes keep their own
src = "shared text"
m = +src
d = m.dup
bs = m.byteslice(0, 6)
sl = m[0, 6]
rg = m[7..]
fz = m.dup.freeze
ts = String.new(m)
hk = {}
hk[m] = 1
m[0] = "S"
m[7, 4] = "TEXT"
m.insert(6, "+")
m.slice!(-1)
p m, src, d, bs, sl, rg, fz, ts, hk.keys, hk[+"shared text"]

# a String that grows well past its room, splice after splice
g = +""
1000.times { |i| g.insert(g.length / 2, (i % 10).to_s) }
g[0, 10] = "-" * 20
200.times { |i| g[i * 3] = "+" }
p g.length, g.count("+"), g.count("-"), g[0, 25], g.sum

# a long String, a splice near the start each time
long = "x" * 50_000
2000.times { |i| long[i * 7] = "ab" }
p long.length, long.count("a"), long[0, 12], long.hash == ("x" * 0 + long).hash

# slice! by each form
t = +"0123456789"
p t.slice!(3), t.slice!(-1), t.slice!(1, 2), t.slice!(2..3), t.slice!(/(6)(7)/, 2), t.slice!("8"), t.slice!(99), show(t)
g = +"0478"
p g.slice!(/(4)(7)/, 2), show(g), g.slice!(/0(4)/, 1), show(g)

# an element, an attribute and a captured local
rows = [+"....", +"...."]
rows[1][2] = "#"
Cell = Struct.new(:text)
cell = Cell.new(+"cell")
cell.text[0] = "C"
cap = +"cap"
fix = -> { cap[0] = "K" }
fix.call
p rows, cell.text, cap

# a receiver the program also holds as another class, and a boxed key
def any(v) = v
x = any(+"boxed")
x = any(1) if ARGV.size > 5
x[0] = "B"
x[1, 2] = "OX"
y = +"keys"
k = ARGV.size > 5 ? 1 : 0.7
y[k] = "K"
kr = ARGV.size > 5 ? 1 : /e(y)/
y[kr] = "E"
kn = ARGV.size > 5 ? 1 : (3..3)
y[kn] = "Z"
p x, y
