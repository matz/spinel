# A Method of a true's or a false's builtin that both define in the same
# module answers that module as its owner, also read back from a container.
a = [true.method(:frozen?), 1]
p a[0].owner
h = {k: false.method(:frozen?), e: true.method(:==), n: false.method(:hash)}
p h[:k].owner, h[:e].owner, h[:n].owner
p [true.method(:frozen?), 2][0].call
m = true.method(:itself)
p m.owner
