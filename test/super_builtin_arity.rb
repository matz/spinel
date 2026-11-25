# A super into a builtin method with a count the builtin cannot take raises
# CRuby's ArgumentError, from each kind of class the super is in.

def try
  r = yield
  p(r == true || r == false ? r : :value)
rescue ArgumentError => e
  p [e.class, e.message]
end

class K
  def respond_to?() = super
  def is_a?(a, b) = super
  def freeze(x) = super
  def ==() = super
  def frozen?(x) = super
  def inspect(x) = super
end
k = K.new
try { k.respond_to? }
try { k.is_a?(K, 1) }
try { k.freeze(1) }
try { k.__send__(:==) }
try { k.frozen?(1) }
try { k.inspect(1) }

class E < StandardError
  def message(x) = super
  def respond_to?() = super
end
e = E.new("m")
try { e.message(1) }
try { e.respond_to? }

S = Struct.new(:a) do
  def to_s(x) = super
  def is_a?() = super
end
s = S.new(1)
try { s.to_s(1) }
try { s.is_a? }

D = Data.define(:a) do
  def with(x) = super
end
try { D.new(a: 1).with(2) }

# the counts it takes still answer
class Ok
  def respond_to?(m, all = false) = super
  def is_a?(k) = super
end
o = Ok.new
try { o.respond_to?(:zzq) }
try { o.respond_to?(:freeze, true) }
try { o.is_a?(Ok) }

# a class method's super into Class's superclass, subclasses or ancestors:
# a bare super passes the method's parameters, which those take none of
def run
  yield
rescue ArgumentError, TypeError => e
  puts "#{e.class} #{e.message}"
end

class CK; end
class CL < CK
  def self.superclass(x = 1); super; end
  def self.subclasses(*a, &b); super; end
  def self.ancestors(k: 1); super; end
end
class CM < CL
  def self.superclass(*a); a.empty? ? super : [:given, *a]; end
end
class CN < CK
  def self.superclass(...); super; end
  def self.ancestors(**kw); super.first(2); end
end
run { p CL.superclass }
run { p CL.superclass(2) }
run { p CL.subclasses }
run { p(CL.subclasses { 1 }) }
run { p CL.subclasses(*[1, 2]) }
run { p CL.ancestors }
run { p CM.superclass, CM.superclass(1) }
run { p [CN.superclass, CN.superclass] }
run { p CN.ancestors }
run { p CN.ancestors(a: 1) }
class CE < CK
  def self.superclass(x) = super(x)
  def self.subclasses(*a) = super(*a)
end
run { p CE.superclass(1) }
run { p CE.subclasses, CE.subclasses(*[nil]) }
# (a module's method a class extends with runs on the class)
module CF
  def superclass(*a) = super
end
class CX < CK
  extend CF
end
run { p CX.superclass }
run { p CX.superclass(1) }
# (a module has no subclasses: CRuby's NoMethodError)
module CMo
  def self.subclasses = super
end
begin
  p CMo.subclasses
rescue NoMethodError => nme
  p nme.class
end
