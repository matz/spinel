# A visibility word in a begin body with rescue clauses that runs whenever
# the begin does gives its names that visibility, in a class, in a module
# and on the copies an include or a prepend makes, as the word placed in the
# body itself would, for the names ahead of the first the word raises for;
# one a raise before it skips leaves the name as it was.
module M
  def a = :a
  def b = :b
  begin
    private :a, :zz
  rescue NameError => e
    p e.message
  end
end
class K; include M; end
class S < K; end
class Q; prepend M; end
class E; extend M; end
class C
  def x = :x
  def y = :y
  def self.z = :z
  private :x
  begin
    public :x
  rescue NameError
  end
  begin
    private_class_method :z
  rescue NameError
  end
  begin
    raise ArgumentError, "skip" if ARGV.size < 5
    private :y
  rescue ArgumentError
  end
end
class D
  def m1 = :m1
  def m2 = :m2
  def m3 = :m3
  def m4 = :m4
  begin
    private :zz, :m1
  rescue NameError
  end
  begin
    private :m2, :zz, :m3
  rescue NameError
  end
  begin
    private :m4, (raise ArgumentError, "args")
  rescue ArgumentError
  end
end
class U
  def n1 = :n1
  def n2 = :n2
  def n3 = :n3
  undef_method :n2
  begin
    private :n1, :n2, :n3
  rescue NameError
  end
end
class L
  def l1 = :l1
  def l2 = :l2
  def l3 = :l3
  begin
    private :l4, :l1
  rescue NameError
  end
  def l4 = :l4
  begin
    private :l2
  rescue NameError
  end
  begin
    public :zzz, :l2
  rescue NameError
  end
  begin
    public :l3
  rescue NameError
  end
  [].each { private :l3 }
end
p [L.new.l1, (L.new.l2 rescue $!.class), L.new.l3, L.new.l4]
class Z
  def z1 = :z1
  def z2 = :z2
  def z3 = :z3
  0.times { def zb = :zb }
  ARGV.each { def zc = :zc }
  begin
    private :zb, :z1
  rescue NameError
  end
  begin
    private :zc, :z2
  rescue NameError
  end
  begin
    private :z3
  rescue NameError
  end
end
module Rm; def rb = :rb; end
module Rn; include Rm; end
class Y
  def y1 = :y1
  def y2 = :y2
  def y3 = :y3
  begin; alias_method :ya, :yzz; rescue NameError; end
  (raise "x"; def yb = :yb) rescue nil
  [].each { include Rn }
  begin; private :ya, :y1; rescue NameError; end
  begin; private :yb, :y2; rescue NameError; end
  begin; private :rb, :y3; rescue NameError; end
end
def tb = :tb
class BO < BasicObject
  def ba = :ba
  begin; private :tb, :ba; rescue ::NameError; end
end
BOA = BasicObject
class BC < BOA
  def bc = :bc
  begin; private :tb, :bc; rescue ::NameError; end
end
module Foo
  String = BasicObject
  class SK < String
    def sc = :sc
    begin; private :tb, :sc; rescue ::NameError; end
  end
  Object = BasicObject
  class SO < Object
    def so = :so
    begin; private :tb, :so; rescue ::NameError; end
  end
end
module Pm; def pb = :pb; end
class PC
  prepend Pm
  def pa = :pa
  begin; private :pb, :pa; rescue NameError; end
end
class DA
  def da = :da
  def dz = :dz
  begin; private def dx; :dx; end, :zz, :da; rescue NameError; end
  begin; private :zz, def dy; :dy; end, :dz; rescue NameError; end
end
v = [K.new, nil].first
p [(K.new.a rescue $!.class), (S.new.a rescue $!.class), (Q.new.a rescue $!.class), (E.a rescue $!.class), K.new.b]
p [(v.a rescue $!.class), K.new.respond_to?(:a), M.private_instance_methods(false)]
p [C.new.x, C.new.y, (C.z rescue $!.class), C.private_instance_methods(false)]
p [D.new.m1, (D.new.m2 rescue $!.class), D.new.m3, D.new.m4]
p [(U.new.n1 rescue $!.class), U.new.n3]
p [Z.new.z1, Z.new.z2, (Z.new.z3 rescue $!.class)]
p [Y.new.y1, Y.new.y2, Y.new.y3]
p [BO.new.ba, PC.new.pa, PC.new.pb, DA.new.da, DA.new.dy, DA.new.dz]
p [BC.new.bc]
p [Foo::SK.new.sc, Foo::SO.new.so]
class IA
  begin; private(:ia, def ia; 1; end); rescue NameError; end
end
class I3x
  def i3c = :i3c
  begin; private(def i3d; 1; end, :missing3x, :i3c); rescue NameError; end
end
p [(IA.new.ia rescue $!.class), I3x.new.i3c, (I3x.new.i3d rescue $!.class)]
module MR
  def mra = :mra
  begin; module_function :mra, raise("stop"); rescue RuntimeError; end
end
class MH; include MR; end
p [MH.new.respond_to?(:mra), MR.respond_to?(:mra)]
module MS
  def msa = :msa
  begin; module_function :msa, (raise("x") rescue :msa); rescue RuntimeError; end
end
class HS; include MS; end
p [HS.new.respond_to?(:msa)]
