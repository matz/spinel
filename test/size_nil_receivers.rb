# `size` answers nil where the receiver cannot count itself: an Enumerator
# with no size, a Range of non-numbers, a user class whose size is nil.
# Over a boxed receiver -- a parameter that takes several kinds, an Array's
# element, a block's yielded value -- the call's dispatch held the builtin's
# answer as a plain Integer and raised TypeError for the nil.
def sz(o) = o.size
p sz("ab"), sz([1]), sz({a: 1}), sz(Enumerator.new { |y| y << 1 }), sz(1..3)
p (("a".."c").size rescue $!.class)
objs = ["ab", [1, 2], Enumerator.new { |y| y << 1 }, (1..4)]
objs.each { |o| s = o.size; p [s, s.nil?] }
def each_len(xs) = xs.each { |x| yield x.length }
each_len(["abc", [1]]) { |n| p n }
def yz(xs) = xs.each { |x| yield x.size }
yz(["abc", Enumerator.new { |y| y << 1 }, [1, 2]]) { |n| p [n, n.nil?] }
class Bag; def size = nil; end
yz([Bag.new, "xy"]) { |n| p [n, n.nil?] }
