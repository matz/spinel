# A call that runs before a method's first def finds no method: the def
# opens the method, so a body or block run ahead of it raises
# NoMethodError, and the same call answers the def once it has run.
class C; end
module M; end
class P; end
class K < P; end
class MM; def self.method_missing(n, *a) = [:mm, n]; end

def a_cls = C.c1
p((begin; a_cls; rescue NoMethodError; :nme; end))
def C.c1 = 1
p a_cls

def a_inst = C.new.i1
p((begin; a_inst; rescue NoMethodError; :nme; end))
class C; def i1 = 2; end
p a_inst

f = -> { C.c2 }
p((begin; f.call; rescue NoMethodError; :nme; end))
def C.c2 = 3
p f.call

o = C.new
g = proc { o.i2 }
p((begin; g.call; rescue NoMethodError; :nme; end))
class C; def i2 = 4; end
p g.call

def a_msg = C.c3
begin; a_msg; rescue NoMethodError => e; p e.message; end
def C.c3 = 5
p a_msg

def a_sub = K.c4
p((begin; a_sub; rescue NoMethodError; :nme; end))
def P.c4 = 6
p a_sub

def a_arg(x) = C.new.i3(x)
p((begin; a_arg(1); rescue NoMethodError; :nme; end))
class C; def i3(x) = x * 7; end
p a_arg(1)

def a_mod = M.c5
p((begin; a_mod; rescue NoMethodError; :nme; end))
module M; def self.c5 = 8; end
p a_mod

def a_two = C.c6
p((begin; a_two; rescue NoMethodError; :nme; end))
def C.c6 = 9
p a_two
def C.c6 = 10
p a_two

def a_mm = MM.c7
p a_mm
def MM.c7 = 11
p a_mm

p((begin; C.new.i4; rescue NoMethodError; :nme; end))
class C; def i4 = 12; end
p C.new.i4

# a boxed receiver's dispatch keeps the arm of the class whose def comes
# later, for the class itself, a subclass, a module's includer and a class
# value, and through a Symbol's block
class Q; end
class QS < Q; end
module QM; end
class QI; include QM; end
def a_box(o) = (begin; o.b1; rescue NoMethodError; :nme; end)
p [a_box(Q.new), a_box(QS.new), a_box(1)]
class Q; def b1 = 13; end
p [a_box(Q.new), a_box(QS.new), a_box(1)]

def a_boxm(o) = (begin; o.b2; rescue NoMethodError; :nme; end)
p [a_boxm(QI.new), a_boxm("s")]
module QM; def b2 = 14; end
p [a_boxm(QI.new), a_boxm("s")]

def a_map(xs) = xs.map(&:b3)
p((begin; a_map([Q.new]); rescue NoMethodError; :nme; end))
class Q; def b3 = 15; end
p a_map([Q.new])

def a_boxc(k) = (begin; k.b4; rescue NoMethodError; :nme; end)
p [a_boxc(Q), a_boxc(1)]
def Q.b4 = 16
p [a_boxc(Q), a_boxc(1)]

# a name a reflection reader asks about answers from the final method
# tables, so a call it guards keeps the table's target: it never raises
class R; end
def a_rt = R.respond_to?(:g1) ? R.g1 : :none
p((begin; a_rt; rescue NoMethodError; :raised; end) == :raised)
def R.g1 = 17
p a_rt

def a_md = R.method_defined?(:g2) ? R.new.g2 : :none
p((begin; a_md; rescue NoMethodError; :raised; end) == :raised)
class R; def g2 = 18; end
p a_md

def a_df = defined?(R.g3) ? R.g3 : :none
p((begin; a_df; rescue NoMethodError; :raised; end) == :raised)
def R.g3 = 19
p a_df

# a boxed receiver's dispatch answers what its arm calls: the entry's
# method_missing arm, whatever its answer, comes back as it is
class U
  def method_missing(n, *a) = n == :h1 ? :mm : nil
end
def a_mm(o) = (begin; o.h1; rescue NoMethodError; :nme; end)
p [a_mm(U.new), a_mm(1)]
class U; def h1 = 20; end
p [a_mm(U.new), a_mm(1)]

def a_nil(o) = o.h2
a_nil(U.new)
p :ok
class U; def h2 = 21; end
p a_nil(U.new)
p((begin; a_nil(1); rescue NoMethodError; :nme; end))

# a guard spelled through the send family is a reflection reader too
def a_sd = R.send(:respond_to?, :g4) ? R.g4 : :none
p((begin; a_sd; rescue NoMethodError; :raised; end) == :raised)
def R.g4 = 22
p a_sd

# a class value held in a variable answers what its class-method arm
# calls: the entry's method_missing arm too
class V
  def self.method_missing(n, *a) = :cmm
end
class VD; end
def a_cv(k) = (begin; k.h3; rescue NoMethodError; :nme; end)
p [a_cv(V), a_cv(VD)]
def V.h3 = 23
p [a_cv(V), a_cv(VD)]
