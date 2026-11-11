# A Method's receiver tells two Methods apart by its value, not by how the
# program holds it: the same builtin method of one receiver is equal whether
# the receiver is typed or was read back out of an untyped container, and
# hashes alike (so a Hash key finds it); false and the Integer 0 are two
# receivers although both are a zero word.
s = "abc"
a = [1, 2]
h = { s.method(:upcase) => :up, a.method(:size) => :n, true.method(:to_s) => :t, 3.method(:+) => :plus }
puts "same receiver, typed and read back"
p s.method(:upcase) == [1, s].last.method(:upcase)
p [1, s].last.method(:upcase) == s.method(:upcase)
p s.method(:upcase).eql?([1, s].last.method(:upcase))
p s.method(:upcase).hash == [1, s].last.method(:upcase).hash
p a.method(:size) == [0, a].last.method(:size)
p true.method(:to_s) == [0, true].last.method(:to_s)
p 3.method(:+) == [0, 3].last.method(:+)
p h[[1, s].last.method(:upcase)]
p h[[0, a].last.method(:size)]
p h[[0, true].last.method(:to_s)]
p h[[0, 3].last.method(:+)]
puts "another method, or another receiver"
p s.method(:upcase) == [1, s].last.method(:downcase)
p s.method(:upcase) == [1, "xyz"].last.method(:upcase)
p a.method(:size) == [0, [1, 2]].last.method(:size)
puts "false, nil and 0"
p false.method(:object_id) == 0.method(:object_id)
p [false.method(:object_id)].include?(0.method(:object_id))
p nil.method(:object_id) == 0.method(:object_id)
p false.method(:object_id) == false.method(:object_id)
p false.method(:object_id) == [1, false].last.method(:object_id)
p [1, false].last.method(:object_id) == 0.method(:object_id)
