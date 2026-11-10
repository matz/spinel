# Range#cover? with a Range argument reads the argument's ends: a Float
# Range or a String Range argument is covered when both ends fit.
ir = 1..10; ie = 1...10; fr = 1.0..2.0; fe = 1.0...2.0; sr = "a".."z"

# Integer Range receiver
p ir.cover?(2..3), ir.cover?(1.5..2.5), ir.cover?("a".."b")
p ir.cover?(1.0..10.0), ir.cover?(1.0...10.5), ir.cover?(1.0...10.0)
p ie.cover?(1.0...10.0), ie.cover?(1.0..9.5), ie.cover?(1.0..10.0), ie.cover?(1..9)
p (..10).cover?(1.0..2.0), (..10).cover?(..2.0), (1..).cover?(..2.0), (1..).cover?(2.0..3.0)

# Float Range receiver
p fr.cover?(1.2..1.5), fr.cover?(1..2), fr.cover?(1..3), fr.cover?("a".."b")
p fr.cover?(1.0...2.0), fr.cover?(1.5..3.0), fr.cover?(2.0..1.0)
p fe.cover?(1.0..2.0), fe.cover?(1.0...2.0), fe.cover?(1...2), fe.cover?(1..2)
p (1.0..).cover?(1.5..), (1.0..).cover?(0.5..1.0), (..2.0).cover?(..1.0), (..2.0).cover?(1.0..)

# String Range receiver
p sr.cover?("b".."c"), sr.cover?("b"..."c"), sr.cover?("a".."zz"), sr.cover?(1..2)
p sr.cover?("y".."b"), sr.cover?("c".."b")
p ("a"..).cover?("b".."c"), ("a"..).cover?(("b"..)), ("a"..).cover?("A".."b")

# ===, include? and member? compare a Range argument as a value
p fr === (1.2..1.5), fr.include?(1.2..1.5), sr === ("b".."c"), sr.include?("b".."c"), sr.member?("b".."c")

# a receiver or an argument that is boxed
def f(r, x) = r.cover?(x)
p f(fr, 1.2..1.5), f(sr, "b".."c"), f(ie, 1.0..9.5), f(ir, 1.5..2.5)
vals = [ir, fr, sr]
args = [2..3, 1.2..1.5, "b".."c"]
vals.each { |r| p args.map { |a| r.cover?(a) } }
p [fr, ir].map { |r| r.cover?(1.5..2.0) }, [ir].map { |r| r.cover?(1.0...11.0) }
p [sr].map { |r| r.cover?("b"..."c") }, [ir, fr].map { |r| r.cover?(1.5) }, [sr].map { |r| r.cover?("b") }

# an Integer end past 2**53 compares exactly with a Float one
a = 9007199254740993      # 2**53 + 1
f = 9007199254740992.0    # 2**53
p (0.0..f).cover?(a..a), (0.0..f).cover?(0..a - 1), (0..a).cover?(1.0..f), (0...a).cover?(1.0..f)
p (a..a + 1).cover?(f..f), (0..a - 1).cover?(1.0..f), (0...a - 1).cover?(1.0..f)
