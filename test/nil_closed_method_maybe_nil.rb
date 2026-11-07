# A receiver that may be nil, where the program gives nil a private method
# (a top-level def, Kernel's or BasicObject's) or a protected one of
# NilClass's, and the receiver's class has a public method of the name:
# nil raises NoMethodError, as CRuby does. send skips the visibility.
def helper = :top
module Kernel; private def kpriv = :kernel; end
class BasicObject; private def bpriv = :basic; end
class NilClass; protected def prot = :nil; end
class Object; protected def oprot = :obj; end
class K
  def helper = :k
  def kpriv = :k
  def bpriv = :k
  def prot = :k
  def oprot = :k
  def run(z) = (z.helper rescue $!.message)
end
class NilClass
  def inside(z) = (z.prot rescue $!.message)
end
def pick(f) = f ? K.new : nil
x = pick(ARGV.size > 5)
y = pick(ARGV.size < 5)
p((x.helper rescue $!.message))
p((x.kpriv rescue $!.message))
p((x.bpriv rescue $!.message))
p((x.prot rescue $!.message))
p((x.oprot rescue $!.message))
p y.helper, y.kpriv, y.bpriv, y.prot, y.oprot
p K.new.run(x)
p nil.inside(x)
def m(v) = v.helper
p((m(x) rescue $!.class))
begin
  x.helper
rescue NoMethodError => e
  p e.message
end
p((x.send(:prot) rescue $!.message))
# NilClass's own builtin to_s answers before the top-level def; a by-value
# Struct answer from K builds
def to_s = "top"
class K; def to_s = "k"; end
p((x.to_s rescue $!.message))
Pt = Struct.new(:a, :b)
def pt = :top
class K; def pt = Pt.new(1, 2); end
p y.pt.a
# K has no method of the name: nil's private one still names nil
class NilClass; private def only_nil = :n; end
class L; end
z = ARGV.size > 5 ? L.new : nil
p((z.only_nil rescue $!.message))
