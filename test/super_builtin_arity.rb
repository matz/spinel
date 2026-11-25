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
