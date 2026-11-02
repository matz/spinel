# A def in a class_eval, instance_eval or module_eval block is a version of
# its method like a class body's: a call answers the version live when it
# runs. (Each method has a name of its own: a name defined on two classes
# keeps the static answer.)
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
