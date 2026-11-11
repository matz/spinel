# A boxed receiver's String arm runs String's own method of a name the
# program's Object (or Kernel) method has too: its Integer is String's,
# which the dispatch's nil-able slot takes as it is, while an Integer, a
# Float and nil reach the program's method and its nil.
class Object
  def bytesize = $nb
end
class Foo
  def bytesize = $nf
end
$nb = nil
$nf = nil
xs = ["abc", Foo.new, 5, 2.5, nil]
xs.each { |x| p x.bytesize }
$nb = 4
$nf = 9
xs.each { |x| r = x.bytesize; p r.nil? ? :none : r + 1 }
module Kernel
  def length = $nk
end
$nk = nil
["abc", Foo.new, 5].each { |x| p x.length }
$nk = 7
["abc", Foo.new, 5].each { |x| p x.length }
