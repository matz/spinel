# A numeric compared with a container or a program object (an Array, a Hash,
# an object with no <=>): CRuby's ArgumentError, naming the operand's class,
# where the compare was emitted as a C comparison of a double with a
# pointer and the C did not build.
a = [1]
begin
  p(1.0 > a)
rescue ArgumentError => e
  p e.message
end
h = { a: 1 }
begin
  p(1 < h)
rescue ArgumentError => e
  p e.message
end
class K; end
begin
  p(2 >= K.new)
rescue ArgumentError => e
  p e.message
end
p(1.0 > 0.5)
