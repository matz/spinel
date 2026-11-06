# A Method made where respond_to_missing? answers for a name no method
# defines calls method_missing with the arguments, keywords and block.
class Ghost
  def initialize(tag) = @tag = tag
  def real(x) = [:real, x]
  def method_missing(name, *args, **kw, &blk)
    name == :echo ? [@tag, name, args, kw, blk ? blk.call : nil] : super
  end
  def respond_to_missing?(name, include_private = false)
    name == :echo || super
  end
end

# method_missing hands the block on
class Fwd
  def helper(x) = yield(x)
  def method_missing(name, *args, &blk) = name == :twice ? helper(args.first, &blk) : super
  def respond_to_missing?(name, include_private = false) = name == :twice || super
end

g = Ghost.new(:g)
m = g.method(:echo)
p m.call(1, 2)
p m.call(3, k: 4) { :blk }
p m.(5)
p m[6]
p Fwd.new.method(:twice).call(4) { |x| x * 10 }

# reflection
p m.arity
p m.parameters
p m.name
p m.original_name
p m.owner
p m.receiver.equal?(g)
p m.source_location
p m.super_method
p m.inspect
p m.unbind.inspect

# equality
p m == g.method(:echo)
p m.eql?(g.method(:echo))
p m.hash == g.method(:echo).hash
p m == Ghost.new(:h).method(:echo)

# to_proc and &m
pr = m.to_proc
p pr.call(7)
p [1, 2].map(&m)
p pr.lambda?

# beside real Methods, in an Array and a Hash
ms = [g.method(:real), g.method(:echo)]
p ms.map { |x| x.call(8) }
p ms.map(&:arity)
h = { r: g.method(:real), e: g.method(:echo) }
p h[:e].call(9)
p h.values.map(&:owner)

# a false answer raises NameError, at the capture
begin
  g.method(:nothing)
rescue NameError => e
  p [e.class, e.name]
end

# a super in respond_to_missing? answers false
class Plain
  def method_missing(name, *args) = name == :a ? :mm : super
  def respond_to_missing?(name, include_private = false) = super
end
begin
  Plain.new.method(:a)
rescue NameError => e
  p [e.class, e.name]
end

# a name a Kernel or BasicObject method answers is not method_missing's; =~
# is no longer one, so method_missing answers it
class AnyName
  def method_missing(name, *args) = [:mm, name]
  def respond_to_missing?(name, include_private = false) = true
end
eq = AnyName.new.method(:=~)
p eq.call(1)
p [eq.name, eq.arity, eq.owner]

