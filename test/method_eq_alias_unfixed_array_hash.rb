# Two Methods of one builtin method are equal whichever name made them, also
# on an Array or Hash whose element kinds no write fixed (`x = []`) and on a
# boxed receiver that may be an Array, a Hash or a String.
x = []
h = {}
p x.method(:size) == x.method(:length)
p x.method(:append) == x.method(:push)
p x.method(:map) == x.method(:collect)
p x.method(:size).eql?(x.method(:length))
p x.method(:size).hash == x.method(:length).hash
p x.method(:first) == x.method(:last)
p x.method(:size) == [].method(:length)
p h.method(:size) == h.method(:length)
p h.method(:store) == h.method(:[]=)
p h.method(:key?) == h.method(:has_key?)
p h.method(:key?) == h.method(:value?)
p({ x.method(:size) => 1 }[x.method(:length)])
[[], {}, "s"].each do |v|
  p v.method(:size) == v.method(:length)
  p v.method(:to_s) == v.method(:inspect)
end
