# A Method of a true or a false, read back out of a container, answers the
# value it is bound to as its #receiver.
a = [true.method(:frozen?), false.method(:frozen?), 1]
p a[0].receiver, a[1].receiver
h = {t: true.method(:nil?), f: false.method(:&)}
p h[:t].receiver, h[:f].receiver
p a[0].call
