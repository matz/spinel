# A Method made of a method a later include's copy stands in front of
# holds the method that runs when it is made: the superclass's before the
# include has run, the copy's module's after, and none where the class
# has no method of the name before then (Kernel#method's NameError).
module V; def a = 1; end
class P; def a = :p; end
class K < P; end
k = K.new
m = k.method(:a)
class K; include V; end
n = k.method(:a)
p [m.owner, m.call, n.owner, n.call]

module W; def b = 2; end
class L; end
l = L.new
begin
  l.method(:b)
rescue NameError => e
  p e.message
end
class L; include W; end
p [l.method(:b).owner, l.method(:b).call]
