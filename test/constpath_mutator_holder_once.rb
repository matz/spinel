# A String mutator or freeze on a constant read off a Class value that runs
# code (`which::BUF.upcase!`) evaluates the value once, and changes the
# String of the class it answered, in value position (a break's value, an
# assignment, an argument) as in statement position. Each class keeps its
# own constant: the value of the holder must not be read again to pick the
# String the result is taken from.
class A; BUF = +"aa"; end
class B; BUF = +"bb"; end
class C; BUF = +"cc"; end
class D; BUF = +"dd"; end
class E; BUF = +"ee"; end
class F; BUF = +"ff"; end

$n = 0
$pair = [A, B]
def which
  $n += 1
  puts "which #{$n}"
  $pair[($n - 1) % 2]
end

def reset(pair)
  $n = 0
  $pair = pair
end

puts "-- freeze in value position"
reset([A, B])
p which::BUF.freeze
p A::BUF.frozen?, B::BUF.frozen?
x = [1].each { break which::BUF.freeze }
p x, A::BUF.frozen?, B::BUF.frozen?
y = which::BUF.freeze
p y, A::BUF.frozen?, B::BUF.frozen?

puts "-- freeze in statement position"
reset([E, F])
which::BUF.freeze
p E::BUF.frozen?, F::BUF.frozen?

puts "-- upcase! in value position"
reset([C, D])
x = [1].each { break which::BUF.upcase! }
p x, C::BUF, D::BUF
reset([D, C])
y = which::BUF.downcase!
p y, C::BUF, D::BUF
p which::BUF.upcase!
p C::BUF, D::BUF

puts "-- mutators in statement position"
reset([C, D])
which::BUF << "!"
p C::BUF, D::BUF
which::BUF.concat("?")
p C::BUF, D::BUF
which::BUF.upcase!
p C::BUF, D::BUF
reset([D, C])
which::BUF.swapcase!
p C::BUF, D::BUF

puts "-- a mutator that changes nothing answers nil"
reset([C, D])
z = which::BUF.upcase!
p z, C::BUF, D::BUF

puts "-- a holder with no side effect"
k = C
p k::BUF.downcase!
p k::BUF
p [k][0]::BUF.upcase!
p C::BUF

puts "-- the holder runs where the receiver is evaluated, not ahead of operands Ruby evaluates first"
class G; BUF = +"aa"; end
class H; BUF = +"bbbb"; end
$k = G
def cur = $k
p "#{($k = H; 1)} #{cur::BUF.size}"
$k = G
p "#{($k = H; 1)} #{cur::BUF.upcase!}"
$k = G
s = +"s"
s.concat(($k = H; "x"), cur::BUF.downcase!.to_s)
p s, G::BUF, H::BUF

puts "-- a safe-navigation call on the result"
reset([C, D])
q = which::BUF.downcase!&.size
p q

puts "-- a String []= through the holder runs it once"
class M; BUF = +"mm"; end
class N; BUF = +"nn"; end
reset([M, N])
r = []
r << (which::BUF[0] = "Z")
p r, $n, M::BUF, N::BUF
x = (which::BUF[0, 1] = "Y")
p x, $n, M::BUF, N::BUF
which::BUF[1] = "X"
p $n, M::BUF, N::BUF

puts "-- a program's own String bang method, a safe-navigation call, an encoding change"
class String
  def twice! = replace(self * 2)
end
class P; BUF = +"pp"; end
class Q; BUF = +"qq"; end
reset([P, Q])
x = which::BUF.twice!
p x, P::BUF, Q::BUF, $n
reset([P, Q])
y = which::BUF&.upcase!
p y, P::BUF, Q::BUF, $n
reset([P, Q])
z = which::BUF.force_encoding("BINARY")
p z.encoding, P::BUF.encoding, Q::BUF.encoding, $n

puts "-- a block call: the statement holds, the value form is master's"
class S; BUF = +"aa"; end
class T; BUF = +"bb"; end
reset([S, T])
which::BUF.gsub!(/a/) { "R" }
p S::BUF, T::BUF, $n
$k = G
p "#{($k = H; 1)} #{cur::BUF.upcase! { }}"
p G::BUF, H::BUF
