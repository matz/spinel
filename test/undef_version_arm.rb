# An undef is a version of the method: where the record cannot tell
# whether it has run, the version entry's arm for it raises when its cell
# says it ran, NameError for a variable-like call and NoMethodError for
# every other form, and the other arms run their versions. A Method's
# capture raises NameError where the undef has run.

# main's singleton: an undef under a run-time condition, Object's method behind it
class Object; def f = "Object#f"; end
def self.f = "main.f"
if ARGV.size == 0
  class << self; undef_method :f; end
end
p((f rescue $!.class))
p((f() rescue $!.class))
p((self.f rescue $!.class))
p((send(:f) rescue $!.class))
p(([1].map { f } rescue $!.class))
# (a Method of it: CRuby's NameError at the capture)
p((method(:f) rescue $!.class))

# ...and one that does not run
def self.f2 = "main.f2"
if ARGV.size == 1
  class << self; undef_method :f2; end
end
p((f2 rescue $!.class))
p((self.f2 rescue $!.class))

# an instance method: the undef stops the lookup only once it has run
class A; def g = 1; end
class B < A
  def g = 2
  def via_vcall = g
  def via_self = self.g
end
if ARGV.size == 0
  class B; undef_method :g; end
end
b = B.new
p((b.g rescue $!.class))
p((b.via_vcall rescue $!.class))
p((b.via_self rescue $!.class))
p((b.send(:g) rescue $!.class))
p((b.method(:g) rescue $!.class))
p A.new.g

class C; def h = 1; end
class D < C; def h = 2; end
if ARGV.size == 1
  class D; undef_method :h; end
end
p((D.new.h rescue $!.class))
2.times do |i|
  p((D.new.h rescue $!.class))
  class D; undef_method :h; end if i == 0
end

# a loop runs the undef after the first call
def self.k = "main.k"
j = 0
while j < 2
  p((k rescue $!.class))
  class << self; undef_method :k; end if j == 0
  j += 1
end

# an undef, then a def again
def self.r = "main.r"
if ARGV.size == 0
  class << self; undef_method :r; end
end
p((r rescue $!.class))
def self.r = "main.r2"
p r

# the arm passes the call's arguments to the error
def self.q(a, b = 2) = [a, b]
class E; def m(x) = x; end
if ARGV.size == 0
  class << self; undef_method :q; end
  class E; undef_method :m; end
end
begin
  q(1)
rescue NoMethodError => e
  p [e.name, e.args, e.message]
end
begin
  E.new.m(5)
rescue NoMethodError => e
  p [e.name, e.args, e.message]
end

# a class method: respond_to? reads the undef's cell
class K; def self.s = 1; end
class J < K; def self.s = 2; end
if ARGV.size == 0
  class << J; undef_method :s; end
end
2.times do
  p J.respond_to?(:s)
  p((J.s rescue $!.class))
end

# a conditional def that did not run: NameError for a variable-like call
if ARGV.size == 1
  def zz = 1
end
p((zz rescue $!.class))
p((zz() rescue $!.class))
p((self.zz rescue $!.class))
