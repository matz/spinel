# spinel: gc-stress
# A literal lambda as a case/in value pattern calls the lambda with the
# subject (Proc#===), boxed or typed, as a `when` lambda does.
def pv(x) = x
pv(1); pv("s"); pv(1.5)
puts(case pv(5); in ->(x) { x > 10 } then 1; else 0; end)
puts(case pv(12); in ->(x) { x > 10 } then 1; else 0; end)
puts(case 5; in ->(x) { x > 10 } then 1; else 0; end)
puts(case 12; in ->(x) { x > 10 } then 1; else 0; end)
puts(case pv("abc"); in ->(s) { s.size == 3 } then 1; else 0; end)
puts(case "abc"; in ->(s) { s.size == 4 } then 1; else 0; end)
lim = 3
puts(case pv(5); in ->(x) { x > lim } then 1; else 0; end)
n = 0
3.times { |i| p(case pv(i); in ->(a) { n += 1; a > 0 } then :y; else :n; end) }
p n
# under a guard; one-line `in` and `=>`
k = 0
p(case pv(4); in ->(v) { k += 1; v > 2 } if k == 1 then :g1; else :no; end)
p(case 1; in ->(v) { v > 2 } if true then :g2; else :no; end)
p(case 5; in ->(v) { v > 2 } unless false then :g3; else :no; end)
p k
p((pv(5) in ->(v) { v > 3 }))
p((pv(1) in ->(v) { v > 3 }))
p((1 in ->(v) { v > 3 }))
begin; pv(1) => ->(v) { v > 3 }; p :nr; rescue NoMatchingPatternError => e; p e.class; end
begin; pv(9) => ->(v) { v > 3 }; p :ok; rescue NoMatchingPatternError => e; p e.class; end
def classify(v)
  case v
  in Integer => n if n < 0 then :neg
  in ->(x) { x.is_a?(Integer) && x > 100 } then :big
  in Integer then :int
  in ->(s) { s.is_a?(String) && s.empty? } then :empty
  in String then :str
  end
end
[pv(-1), pv(500), pv(5), pv(""), pv("x")].each { |e| p classify(e) }

# a Range subject: what the lambda's body reads of its parameter
r = (1..3)
puts(case r; in ->(q) { q.include?(2) } then 1; else 0; end)
puts(case r; in ->(q) { q.first == 2 } then 1; else 0; end)
puts(case (1..3) when ->(q) { q.include?(2) } then 1 else 0 end)
puts(case r when ->(q) { q.first == 1 } then 1 else 0 end)
puts(case r when ->(q) { q.last == 3 && q.first == 1 } then 1 else 0 end)
puts(case (2..5) when ->(q) { q.size == 4 } then 1 else 0 end)
puts(case r when ->(q) { q.to_a.size == 3 } then 1 else 0 end)
puts(case r; in ->(q) { q.last == 3 } then 1; else 0; end)

# a typed object subject, in `when` and `in`
class Pt
  attr_reader :v
  def initialize(v); @v = v; end
  def self.make(v); 300.times { "x" * 8 }; new(v); end
end
quiet = ->(o) { o.v == 3 }
busy  = ->(o) { 200.times { "q" * 64 }; o.v == 3 }
p(case Pt.make(3) when quiet then :quiet else :no end)
s = Pt.make(3)
p(case s when busy then :busy_local else :no end)
p(case Pt.make(3); in ->(o) { o.v == 3 } then :in_hit; else :no; end)
p(case Pt.make(4); in ->(o) { 200.times { "q" * 64 }; o.v == 3 } then :in_hit; else :no; end)
