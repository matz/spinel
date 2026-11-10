# Kernel#respond_to_missing? answers false for a self no program definition
# answers for: receiverless, self., send and __send__, from a class, an
# Object method and main, on typed, boxed and builtin receivers, and as the
# `super` of a program's own definition (a Struct- or Exception-made class's
# too). A program definition still wins wherever one answers.
class Opt; def respond_to_missing?(n, all = false) = n == :opt || super; end
class Req; def respond_to_missing?(n, all) = n == :req || super; end
class Plain
  def ask(n) = respond_to_missing?(n, false)
  def ask_self(n) = self.respond_to_missing?(n, true)
end
module Mix; def respond_to_missing?(n, all = false) = n == :mix || super; end
class Mixed; include Mix; def ask(n) = respond_to_missing?(n, false); end
class Base; def ask(n) = respond_to_missing?(n, false); end
class Derived < Base; def respond_to_missing?(n, all = false) = n == :derived; end
class Other < Base; end
Rec = Struct.new(:v) do
  def respond_to_missing?(n, all = false) = n == :rec || super
end
class Err < StandardError
  def respond_to_missing?(n, all = false) = n == :err || super
end
class Object; def objask(n) = respond_to_missing?(n, false); end

p Plain.new.ask(:x), Plain.new.ask_self(:x)
p Plain.new.send(:respond_to_missing?, :x, false), Plain.new.__send__(:respond_to_missing?, :x, true)
p Opt.new.send(:respond_to_missing?, :opt, false), Opt.new.send(:respond_to_missing?, :zz, false)
p Req.new.send(:respond_to_missing?, :req, false), Req.new.send(:respond_to_missing?, :zz, true)
p Mixed.new.ask(:mix), Mixed.new.ask(:x), Plain.new.ask(:mix)
p Base.new.ask(:derived), Derived.new.ask(:derived), Other.new.ask(:derived)
p [Base.new, Derived.new, Other.new].map { |o| o.ask(:derived) }
p Rec.new(1).send(:respond_to_missing?, :rec, false), Rec.new(1).send(:respond_to_missing?, :zz, false)
p Err.new.send(:respond_to_missing?, :err, false), Err.new.send(:respond_to_missing?, :zz, false)
p 1.objask(:x), "s".objask(:x), nil.objask(:x), Opt.new.objask(:opt), Plain.new.objask(:x)
p 1.send(:respond_to_missing?, :x, false), nil.send(:respond_to_missing?, :x, false), :s.__send__(:respond_to_missing?, :x, true)
mixed = [Opt.new, Plain.new, 1, "s", nil, :sym, Rec.new(2)]
p mixed.map { |x| x.send(:respond_to_missing?, :opt, false) }
p send(:respond_to_missing?, :x, false), self.send(:respond_to_missing?, :x, true)
def top = respond_to_missing?(:x, false)
p top
$log = []
def t(x) = ($log << x; x)
p Plain.new.send(:respond_to_missing?, t(:a), t(false))
p $log
n = 0
200.times { |i| n += 1 if Plain.new.ask("n#{i}".to_sym) }
p n
# the operand count is Kernel's (2), and an explicit receiver names a private method
def err
  yield
rescue => e
  puts "#{e.class}: #{e.message}"
end
class Plain
  def one = respond_to_missing?(:x)
  def three = respond_to_missing?(:x, false, 1)
end
err { Plain.new.one }
err { Plain.new.three }
err { Plain.new.respond_to_missing?(:x, false) }
err { Opt.new.respond_to_missing?(:opt, false) }
# a plain explicit receiver keeps raising where the row answers (a boxed receiver, a base whose subclass defines it)
class Boxed; def respond_to_missing?(n, all = false) = n == :a; end
p [Boxed.new, 1, "s"].map { |x| (x.respond_to_missing?(:a, false) rescue :nm) }
p [Boxed.new, 1, "s"].map { |x| x.send(:respond_to_missing?, :a, false) }
class Base; end
class Sub < Base; def respond_to_missing?(n, a = false) = true; end
[Base.new, Sub.new].each { |o| p((o.respond_to_missing?(:z, false) rescue :nm)) }
[Base.new, Sub.new].each { |o| p o.send(:respond_to_missing?, :z, false) }
# a super's operands run, then its block argument
class Sup; def respond_to_missing?(n, a = false) = super(t(n), t(a), &t(nil)); end
$log = []
p Sup.new.send(:respond_to_missing?, :x, true), $log
# a bare super in a closure reads its parameters through the closure (a proc, a lambda that reassigns one, a proc kept in an ivar)
class ClA
  def respond_to_missing?(n, all = false)
    pr = proc { super }
    n = :changed
    pr.call
  end
end
p ClA.new.send(:respond_to_missing?, :x, true)
class ClB
  def respond_to_missing?(n, all = false)
    l = -> { all = !all; super }
    [l.call, l.call]
  end
end
p ClB.new.send(:respond_to_missing?, :x, false)
class ClC
  def respond_to_missing?(n, all = false)
    @k = proc { super }
    @k.call
  end
end
p ClC.new.send(:respond_to_missing?, :x, false)
# Kernel's arity row is not a singleton method; a block is accepted and ignored
p Kernel.singleton_methods(false).include?(:respond_to_missing?), Kernel.singleton_methods(false).include?(:block_given?)
class WithBlock; def t = respond_to_missing?(:x, false) { }; end
p WithBlock.new.t, WithBlock.new.send(:respond_to_missing?, :x, false) { }
# an operand that raises still raises
class Raises
  def t = respond_to_missing?(1 / 0, false)
  def t2(z) = respond_to_missing?(:x, z / 0)
end
[[:t], [:t2, 0]].each { |m, *a| begin; p Raises.new.__send__(m, *a); rescue => e; p e.class; end }
begin; p Raises.new.send(:respond_to_missing?, 1 / 0, false); rescue => e; p e.class; end
# a descendant-only definition of another arity, the operands run once
$bumped = 0
def bump = ($bumped += 1)
class DBase; def go = respond_to_missing?(:q, bump > 0); end
class DS1 < DBase; def respond_to_missing?(n, a) = a; end
class DS2 < DBase; def respond_to_missing?(n, a, b = 2) = b == 2; end
[DBase.new, DS1.new, DS2.new].each { |x| p x.go }
p $bumped
