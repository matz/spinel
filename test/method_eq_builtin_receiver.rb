# A Method of a builtin method on one receiver is equal to another Method of
# the same method, whichever `.method(:sym)` call made it: eql?, equal? and
# hash agree with ==, and a Hash key finds it.
s = "abc"
a = [1, 2]
p s.method(:upcase) == s.method(:upcase)
p s.method(:upcase).eql?(s.method(:upcase))
p s.method(:upcase).equal?(s.method(:upcase))
p s.method(:upcase) != s.method(:upcase)
p s.method(:upcase).hash == s.method(:upcase).hash
p a.method(:size) == a.method(:size)
p true.method(:to_s) == true.method(:to_s)
p nil.method(:to_s) == nil.method(:to_s)
# a different method, or a different receiver, is a different Method
p s.method(:upcase) == s.method(:downcase)
p 1.method(:+) == 2.method(:+)
p true.method(:to_s) == false.method(:to_s)
p a.method(:size) == [1, 2].method(:size)
# a Method is a Hash key
h = { s.method(:upcase) => :up }
p h[s.method(:upcase)]
p h[s.method(:downcase)]
# not a Method
p s.method(:upcase) == "upcase"
