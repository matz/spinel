# `public_send(*args)` on a receiver whose class is known only at run time
# (self in a reopened Object) passes the rest of the list to a builtin
# method: `upcase` takes none of it, `first` one, `center` two.
# activesupport's Object#try is such a send. The arm read the rest as
# one argument it could not take, and raised NoMethodError for every
# name.
class Object
  def snd(*args) = public_send(*args)
end

p 1.snd(:succ)
p "a".snd(:upcase)
p :b.snd(:upcase)
p [3, 4].snd(:first)
p [3, 4].snd(:first, 1)
p "ab".snd(:center, 6, "*")
begin
  1.snd(:succ, 2)
rescue ArgumentError => e
  p e.message
end

# with a block forwarded beside the list, as Object#try passes one
class Foo
  def snd(x, *args, &blk) = x.public_send(*args, &blk)
end
foo = Foo.new
p foo.snd([1, 2], :map) { |v| v * 3 }
p foo.snd("ab", :upcase)
p foo.snd(5, :succ)
