# String#[]= through a constant, a global or a class variable, in value
# position. A statement stored through the constant already; an assignment
# whose value is read (`x = (K::BUF[0] = "Q")`, an argument, an element) was
# the unresolved-method raise, whatever the index. The value is the assigned
# String, the constant's String changes, and a frozen constant raises
# FrozenError. A holder that is a Class value (`pick::BUF[0] = "Z"`) runs
# once.

puts "-- first and later, value and statement"
class K1; BUF = +"abcdef"; end
x = (K1::BUF[0] = "Q")
y = (K1::BUF[1] = "RR")
K1::BUF[2] = "S"
z = (K1::BUF[-1] = "Z")
p x, y, z, K1::BUF

puts "-- range, regexp and string indexes"
class K2; BUF = +"hello world"; end
a = (K2::BUF[0..1] = "J")
b = (K2::BUF[/o w/] = "0")
c = (K2::BUF["rl"] = "RL")
p a, b, c, K2::BUF

puts "-- three arguments"
class K3; BUF = +"abcdef"; end
a = (K3::BUF[1, 2] = "XYZ")
b = (K3::BUF[/(Z)(d)/, 2] = "D")
p a, b, K3::BUF
r = []
r << (K3::BUF[0, 1] = "q")
p r, K3::BUF

puts "-- a nested path"
module A4; class B4; BUF = +"nested"; end; end
v = (A4::B4::BUF[0] = "N")
A4::B4::BUF[1] = "E"
w = (A4::B4::BUF[-1] = "D")
p v, w, A4::B4::BUF

puts "-- a frozen constant"
class K5; BUF = "frozen"; MUT = +"mut"; end
begin
  x = (K5::BUF[0] = "F")
  p x
rescue FrozenError => e
  puts "FrozenError: #{e.message}"
end
begin
  K5::BUF[1] = "R"
rescue FrozenError => e
  puts "FrozenError: #{e.message}"
end
p K5::BUF, (K5::MUT[0] = "M"), K5::MUT

puts "-- a Class value holder"
class A6; BUF = +"aa"; end
class B6; BUF = +"bb"; end
$n = 0
def pick = ($n += 1; puts "pick #{$n}"; $n.odd? ? A6 : B6)
x = (pick::BUF[0] = "Z")
p x, A6::BUF, B6::BUF
pick::BUF[1] = "Y"
p A6::BUF, B6::BUF
r = []
r << (pick::BUF[0] = "W")
p r, A6::BUF, B6::BUF, $n
k = A6
y = (k::BUF[0] = "V")
p y, A6::BUF

puts "-- a top-level constant, a global, a class variable"
TOP7 = +"top"
$gv7 = +"glob"
class K7
  @@cv = +"cvar"
  def self.go = (@@cv[0] = "C")
end
x = (TOP7[0] = "T")
y = ($gv7[0, 2] = "G")
p x, y, TOP7, $gv7, K7.go

puts "-- as an argument, an element and a hash value"
class K8; BUF = +"abcdef"; end
def show(v) = p(v)
show(K8::BUF[0] = "1")
p [K8::BUF[1] = "2", K8::BUF[2] = "3"]
h = { k: (K8::BUF[3] = "4") }
p h, K8::BUF
t = K8::BUF[4] = "5"
p t
