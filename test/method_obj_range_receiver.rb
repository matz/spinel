# A Method of a Range of Integers: the Range is a value, not a pointer the
# Method's self slot can carry, and binding one cast the struct to a pointer,
# which did not compile (`(1..3).method(:size)`, for any name). It rides in
# the holder a boxed receiver's Method uses, as a Float's does.
r = (1..3)
p r.method(:size).call
p r.method(:first).call
p r.method(:first).call(2)
p r.method(:last).call
p r.method(:to_a).call
p r.method(:include?).call(2)
p r.method(:cover?).call(5)
p r.method(:min).call
p r.method(:max).call
p r.method(:count).call
p r.method(:step).call(2).to_a
p (1...4).method(:size).call
p (1..).method(:first).call(2)

m = r.method(:size)
p m.arity
p r.method(:first).arity
p m.name
p m.owner
p m.receiver
p m.to_proc.call
p m.()

# held in a local, passed on, read back out of an Array, used as a block
held = r.method(:last)
p held.call
def run(meth) = meth.call
p run((1..4).method(:size))
p [(1..2), (1..5)].map { |x| x.method(:size) }.map(&:call)
p [1, 5].map(&(1..3).method(:include?))
h = { r.method(:size) => 1 }
p h.size

# a name a Range does not have is CRuby's NameError, its receiver evaluated
begin
  r.method(:nope_zz)
rescue NameError => e
  puts e.message
end
