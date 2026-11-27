# A send of a computed name on an Array or a Hash may be one of its stores:
# push, []=, concat, store, merge!, map!. What it is handed can be read back
# from the receiver, and a call on it dispatches to its class's method. The
# arguments may come from a splat, a slice assignment and concat put the
# elements of an argument in, and map! puts the block's values in.
class A; def foo = "A"; end
class B; def foo = "B"; end
class C; def foo = "C"; end

m = :push
a = [A.new]
a.send(m, B.new)
a.each { |v| puts v.foo }

m = :[]=
a2 = [A.new]
a2.send(m, 0, C.new)
a2.each { |v| puts v.foo }

a3 = [A.new]
a3.send(m, 1, 0, [B.new])
a3.each { |v| puts v.foo }

m = :concat
a4 = [A.new]
a4.send(m, [C.new])
a4.each { |v| puts v.foo }

m = :push
args = [B.new, C.new]
a5 = [A.new]
a5.send(m, *args)
a5.each { |v| puts v.foo }

m = :store
h = { a: A.new }
h.public_send(m, :b, B.new)
h.each_value { |v| puts v.foo }

m = :merge!
h2 = { a: A.new }
h2.send(m, { b: C.new })
h2.each_value { |v| puts v.foo }

m = :map!
a6 = [A.new]
a6.__send__(m) { B.new }
a6.each { |v| puts v.foo }
