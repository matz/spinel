# A send by a name held in a variable, with a nil argument, at the end: its
# arms are the methods named by the program's Symbol literals. The wrapper
# `12.method(:to_s)` desugars to is named by one, so an arm called it with
# the nil, and the wrapper's receiver slot took its nil beside the value.
# The call through the Method passed the receiver as one register where the
# wrapper read two, and the nil flag came from whatever the second held (on
# x86-64, "" for to_s). `two`'s first parameter does hold its nil, through
# the Method and through a boxed Method.
mt = 12.method(:to_s)
p mt.call

def two(a, b) = [a, b]
m2 = method(:two)
p m2.call(3, 4)
p m2.call(nil, 5)

def hold(m) = m
mp = hold(ARGV.size > 0 ? 1 : 7.method(:+))
p mp.call(2)
mq = hold(ARGV.size > 0 ? 1 : 12.method(:to_s))
p mq.call

nm = :frozen?
begin
  1.public_send(nm, nil)
rescue ArgumentError
end
