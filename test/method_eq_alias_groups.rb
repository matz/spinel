# A typed Method and one whose receiver was read back out of a container are
# one Method when they name one method of one receiver, and that holds across
# the spellings of an alias group (size and length): equality is transitive,
# the hashes agree, and a Hash key finds the Method whichever side made it.
# A typed array's push adapter and a Symbol receiver follow the same rules.
a = [1, 2]
words = ["x", "y"]
sym = :abc
back = [0, a].last
h = { a.method(:size) => :n, a.method(:push) => :push, words.method(:push) => :wpush, sym.method(:to_s) => :sym }
puts "alias spellings, typed and read back"
p a.method(:size) == a.method(:length)
p a.method(:size) == back.method(:size)
p a.method(:size) == back.method(:length)
p a.method(:length) == back.method(:size)
p back.method(:size) == a.method(:length)
p a.method(:size).eql?(back.method(:length))
p a.method(:size).hash == back.method(:length).hash
p h[a.method(:length)]
p h[back.method(:size)]
p h[back.method(:length)]
puts "another method of the same receiver"
p a.method(:size) == back.method(:first)
p a.method(:size).hash == a.method(:first).hash
puts "a typed array's push"
p a.method(:push) == [0, a].last.method(:push)
p a.method(:push).hash == [0, a].last.method(:push).hash
p h[[0, a].last.method(:push)]
p h[[0, words].last.method(:push)]
p a.method(:push) == words.method(:push)
puts "a Symbol receiver"
p sym.method(:to_s) == [0, sym].last.method(:to_s)
p h[[0, sym].last.method(:to_s)]
p sym.method(:to_s).receiver
p sym.method(:to_s) == :abd.method(:to_s)
p sym.method(:to_s) == 1.method(:to_s)
puts "a Symbol and an Integer share no receiver"
x = :a.method(:object_id)
hits = []
(0..200).each { |k| hits << k if x == k.method(:object_id) }
p hits
