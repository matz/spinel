# A Method of a class method a method body defines raises NameError before
# that body runs, as the method does not exist yet, and captures the method
# after it, whether the capture runs in a method or at the top level

class P; end
def set_p = (def P.k = :kp)
def cap_p = (P.method(:k).call rescue $!.class)
def msg_p = (P.method(:k) rescue $!.message)
p cap_p, msg_p
p((P.method(:k).call rescue $!.class))
set_p
p cap_p, msg_p.call, msg_p.name

# beside respond_to?, which answers the same way
class Q; end
def set_q = (def Q.k = :kq)
def look_q
  a = Q.respond_to?(:k)
  b = begin; Q.method(:k).call; rescue NameError => e; e.class; end
  c = begin; Q.public_method(:k).call; rescue NameError => e; e.class; end
  [a, b, c]
end
p look_q
set_q
p look_q

# a subclass's capture through its own class
class R; end
class R2 < R; end
def set_r = (def R.k = :kr)
def cap_r = (R2.method(:k).call rescue $!.message)
p cap_r
set_r
p cap_r

# an instance method of the name elsewhere, and readers that report it,
# leave the class side's capture and respond_to? alone
class U
  def k = :ku
end
class V; end
def set_v = (def V.k = :kv)
def look_v = [V.respond_to?(:k), (V.method(:k).call rescue $!.class), U.new.respond_to?(:k), U.method_defined?(:k)]
p look_v
set_v
p look_v

# a Class.new class is named by the constant written with it
KC = Class.new
def set_kc = (def KC.k = :kc)
def cap_kc = (KC.method(:k).call rescue [$!.class, $!.message])
p cap_kc
set_kc
p cap_kc

# an operator's name, which holds a `%`
def set_kp = (def KC.%(x) = :pc)
def cap_kp = (KC.method(:%).call(1) rescue [$!.class, $!.message])
p cap_kp
set_kp
p cap_kp

# a module's respond_to? and call answer as the body has run
module MM
  def self.other = 1
end
def set_mm = (def MM.k = :km)
def look_mm = [MM.respond_to?(:k), (MM.k rescue $!.class)]
p look_mm
set_mm
p look_mm
