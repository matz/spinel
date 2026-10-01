# A String mutator whose receiver is an expression answering an existing
# String mutates that String, as in CRuby: a conditional, parentheses, an
# assignment, `||`, and the methods that answer their receiver (to_s,
# to_str, itself, tap, a `then` whose block answers its parameter). The
# mutator ran on a copy and the String was left as it was.

c = ARGV.size > 5
g = +"h"; (c ? +"z" : g) << "-"; p g
s = +"ab"; s.to_s << "T"; s.tap { |z| z << "P" }; p s
s1 = +"a"; (s1) << "1"; p s1
s2 = +"a"; s2.itself << "2"; p s2
s3 = +"a"; s3.to_str << "3"; p s3
s4 = +"a"; (c ? +"q" : s4).upcase!; p s4
s5 = +"a"; (if c then +"q" else s5 end) << "5"; p s5
s7 = +"a"; (s7 || +"z") << "7"; p s7
s8 = +"a"; (x = s8) << "8"; p s8, x
s9 = +"a"; s9.then { |z| z } << "9"; p s9
s10 = +"a"; s10.to_s.to_s << "10"; p s10
s11 = +"a"; s11.tap { }.upcase!; p s11
s12 = +"a"
case c
when true then +"q"
else s12
end << "12"
p s12

class A
  def initialize = (@i = +"a")
  def add(x) = (@buf ||= +"") << x
  def buf = @buf
  def tern(c) = ((c ? +"q" : @i) << "i"; @i)
end
a = A.new; a.add("p"); a.add("q"); p a.buf, a.tern(c)
t = nil; (t ||= +"z") << "2"; p t

# an argument that writes a variable, and a block that assigns the
# receiver's variable, keep the receiver's own String
e = +"a"; r = e.to_s << (e = +"b"); p [r, e]
f = +"a"; f.tap { f = +"z" }.upcase!; p f
g = +"a"; r = g.then { |z| g = +"y"; z } << "!"; p [r, g]
