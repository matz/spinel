# A Time, Complex or Rational parameter with a nil default holds nil when the
# caller leaves it out: == nil, class, is_a?, inspect and to_s see the nil.
def eq(t = nil) = t == nil
def cls(t = nil) = t.class
def isa(t = nil) = t.is_a?(Time)
def ins(t = nil) = t.inspect
def str(t = nil) = "<#{t}>"
p eq(Time.at(0)), eq
p cls(Time.at(0)), cls
p isa(Time.at(0)), isa
p ins(Time.at(0).utc), ins
p str(Time.at(0).utc), str
def orr(t = nil) = (t || Time.at(5)).to_i
p orr, orr(Time.at(7))
def add(t = nil); return :none unless t; (t + 60).to_i; end
p add, add(Time.at(0))
def kw(t: nil) = [t.class, t.inspect]
p kw, kw(t: Time.at(0).utc)
def num(x = nil) = [x.class, x == nil, x.inspect]
p num, num(Rational(1, 3)), num(Complex(1, 2))
class K; def self.m(t = nil) = t.nil? ? "nil" : t.utc.to_s; end
p K.m, K.m(Time.at(0))
pr = proc { |t = nil| t.class }
p pr.call, pr.call(Time.at(0))
def rng(t = nil) = (t..Time.at(9)).cover?(Time.at(5))
p rng(Time.at(3)), rng
def rng2(a = nil, b = nil) = (a..b).cover?(Time.at(5))
p rng2(Time.at(1), Time.at(9)), rng2(Time.at(6), Time.at(9)), rng2, rng2(nil, Time.at(9))
def always(t = nil) = (t - Time.at(0)).to_i
p always(Time.at(42))
def rsp(t = nil) = [t.respond_to?(:year), t.respond_to?(:each)]
p rsp(Time.at(0)), rsp
# (nil..nil) covers every value, but not an empty Range given to cover?
def rng3(a = nil, b = nil) = (a..b)
x = rng3
p x.cover?("z".."a"), x.cover?(1..0), x.cover?(5), rng3(Time.at(1), Time.at(9)).cover?(Time.at(5))
# a parenthesized end is still the boxed parameter (not an Integer under promote)
def pend(t = nil) = ((t)..nil).begin
p pend(Time.at(0)).class, pend
