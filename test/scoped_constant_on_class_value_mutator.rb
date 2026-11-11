# `klass::BUF << x` on a constant that a Class value names: the class the
# value carries owns the constant, so the mutation lands in that class's
# String. Two owners were a C error, and a value of a class that owns none
# appended to the one class that did (the constant was found by its leaf
# name alone).

class A; BUF = +"o"; end
class B; BUF = +"a"; end
k = B
k::BUF << "y"
p A::BUF, B::BUF

# the same through a loop, a method result and a parameter
[A, B].each { |c| c::BUF << "y" }
p A::BUF, B::BUF
def pick = B
pick::BUF << "z"
p B::BUF
def grow(c) = (c::BUF << "!")
grow(A)
grow(B)
p A::BUF, B::BUF

# a class that owns none is NameError, and the owner is left alone
class Empty; end
c = Empty
begin
  c::BUF << "n"
rescue NameError => e
  p e.message
end
p A::BUF, B::BUF

# one owner: a Class value that is not it
class One; LONE = +"x"; end
class Other; end
v = Other
begin
  v::LONE << "y"
rescue NameError => e
  p e.message
end
p One::LONE

# every mutator that changes the String in place
class C1; M = +"oo"; end
class C2; M = +"ab"; end
[C1, C2].each do |c|
  c::M << "x"
  c::M.concat("y")
  c::M.upcase!
  c::M.replace(c::M + "z")
  c::M[0] = "Q"
  c::M.prepend("p")
  c::M.insert(1, "i")
  c::M.sub!("X") { "0" }
end
p C1::M, C2::M
[C1, C2].each { |c| c::M.clear }
p C1::M, C2::M

# the receiver runs once
def which(n) = (puts "which#{n}"; n == 1 ? A : B)
which(1)::BUF << "1"
which(2)::BUF << "2"
p A::BUF, B::BUF

# a read, a copy and an inherited constant
x = k::BUF
x << "copy"
p B::BUF, k::BUF.size
class Base; INH = +"b"; end
class Sub < Base; end
s = Sub
s::INH << "s"
p Base::INH

# a class value among others in a poly slot, and a non-String constant
class Arr1; LST = [1]; end
class Arr2; LST = [2]; end
a2 = Arr2
a2::LST << 3
p Arr1::LST, Arr2::LST
cs = [A, 3].first
cs::BUF << "p"
p A::BUF

# a constant a program can make a non-String is read boxed: the Array#pack
# buffer, written through the Class value, and the bounds of clamp
class P1; BUF = ARGV.empty? ? +"" : 1; end
kp = P1
[65].pack("C", buffer: kp::BUF)
p P1::BUF
opts = {}
[66].pack("C", **opts, buffer: kp::BUF)
p P1::BUF
class P2
  LO = ARGV.empty? ? +"b" : 1
  HI = ARGV.empty? ? +"d" : 2
end
kc = P2
r = "a".clamp(kc::LO, kc::HI)
p r, r.equal?(P2::LO)
r << "!"
p P2::LO
s = "z".clamp(kc::LO, kc::HI)
p s.equal?(P2::HI)
