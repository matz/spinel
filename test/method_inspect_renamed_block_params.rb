# Method#inspect and #parameters of a define_method name its block's
# parameters as the program wrote them, though a local of the same name
# elsewhere in the program gave the parameter a slot of its own.
def other
  a = 5
  b = 6
  k = 7
  r = 8
  [a, b, k, r]
end

class C
  define_method(:req) { |a| a + 1 }
  define_method(:opt) { |a, b = 2| a + b }
  define_method(:kw) { |a, k: 3| a + k }
  define_method(:kwreq) { |k:| k }
  define_method(:rest) { |a, *r| [a, r] }
  define_method(:two) { |a, b| [a, b].tap { |a| a } }
  define_method(:nest) { |a| [1].map { |b| a + b } }
end

# the file name varies with where the test runs
def desc(m) = m.inspect.sub(/ \S+:\d+>\z/, ">")

c = C.new
p other
%i[req opt kw kwreq rest two nest].each do |n|
  puts desc(c.method(n))
  p c.method(n).parameters
end
puts desc(C.instance_method(:opt))
p c.req(1), c.opt(1), c.kw(1, k: 4), c.kwreq(k: 9), c.rest(1, 2, 3), c.two(1, 2), c.nest(1)

# a block parameter and a keyword rest, renamed apart or repeated
def other2
  b = 1; r = 2; o = 3
  [b, r, o]
end
class C
  define_method(:wb) { |r, &b| b ? b.call(r) : r }
  define_method(:wr) { |*r, **o| [r, o] }
end
def du(_a, *_a, **_a, &_a) = 1
p other2, c.wb(2) { |x| x * 3 }, c.wb(4), c.wr(1, z: 2), du(1)
puts desc(c.method(:wb)), desc(c.method(:wr)), desc(method(:du))
p c.method(:wb).parameters, c.method(:wr).parameters, method(:du).parameters

# copies of a define_method: an include and a module_function
def other3
  s = nil
  a = 1
  [s, a]
end
module M
  define_method(:ms) { |s| s.nil? ? "nil-s" : s.upcase }
end
class C
  include M
end
module F
  module_function
  define_method(:fm) { |a| a + 1 }
end
p other3, c.ms("a"), F.fm(1)
puts desc(C.instance_method(:ms)), desc(c.method(:ms)), desc(F.method(:fm))
