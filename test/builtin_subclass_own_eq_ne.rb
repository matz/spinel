# != on an Array or String subclass with its own == runs that ==
# (BasicObject#!= calls == and negates it), as on a Hash subclass.
module EqM; def ==(o) = o.is_a?(Integer); end
class A2 < Array; def ==(o) = (o.size == 9 ? nil : 1); end
class A3 < A2; end
class A4 < Array; include EqM; end
class A6 < Array; def eqq(o) = true; alias == eqq; end
class A7 < Array; def ==(o) = false; end
class S2 < String; def ==(o) = o == "zz"; end
class S3 < S2; end
class H2 < Hash; def ==(o) = false; end
a = A2.new([1, 2])
p a != [1], a != Array.new(9, 0)
p A3.new([1]) != [1]
p A4.new([1]) != 5, A4.new([1]) != [1]
p A6.new([1]) != [7]
b = A7.new([1])
p b != b, b != [1], b == b
p S2.new("a") != "zz", S3.new("a") != "a", S2.new("a") != "a"
c = H2.new
p c != c
def mk = A7.new([5])
p mk != [5]
p A7.new([1]).!=([1])
