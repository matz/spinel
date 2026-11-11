# Two Methods of one builtin method are equal whichever of its names made
# them: an alias names the same method, so `size` and `length` are one Method
# and so are `to_s` and `inspect` where CRuby defines the second as the first.
a = [1, 2]
s = "abc"
p a.method(:size) == a.method(:length)
p a.method(:size).eql?(a.method(:length))
p a.method(:size).hash == a.method(:length).hash
p s.method(:size) == s.method(:length)
p true.method(:to_s) == true.method(:inspect)
p false.method(:to_s) == false.method(:inspect)
p false.method(:^) == false.method(:|)
p true.method(:&) == true.method(:^)
p nil.method(:to_s) == nil.method(:inspect)
p s.method(:to_s) == s.method(:to_str)
p s.method(:upcase) == s.method(:downcase)
p a.method(:size) == [1, 2].method(:length)
h = { a.method(:size) => :n }
p h[a.method(:length)]
p a.method(:size).unbind == a.method(:length).unbind
