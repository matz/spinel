# A def in a class_eval, instance_eval or module_eval block is a version of
# its method like a class body's: a method that calls it before the block
# runs raises NoMethodError, and after it answers; with an earlier def, the
# call answers the version live when it runs. (Each method has a name of
# its own: a name defined on two classes keeps the static answer.)
class C; end
def go = C.new.late_c
p((begin; go; rescue NoMethodError; :nme; end))
C.class_eval { def late_c = 40 }
p go

class D; end
def go_d = D.new.late_d
go_d if ARGV.size > 5
D.class_eval { def late_d = 49 }
p go_d

class E; end
def go_e = E.late_e
p((begin; go_e; rescue NoMethodError; :nme; end))
E.instance_eval { def late_e = 55 }
p go_e

module M; end
class F; include M; end
def go_f = F.new.late_f
go_f if ARGV.size > 5
M.module_eval do
  def late_f = 56
end
p go_f

class G; def late_g = 0; end
def go_g = G.new.late_g
p go_g
G.class_eval { def late_g = 58 }
p go_g

class H; def self.late_h = 1; end
def go_h = H.late_h
p go_h
H.instance_eval { def late_h = 2 }
p go_h

class Q; def late_q = 3; end
def go_q = Q.new.late_q
p go_q
Q.module_eval do
  def late_q = 4
end
p go_q
