# A Symbol a boxed value holds, passed as `&f` to an Array#concat the
# program reopens to call its block: the reopen calls the Symbol's proc.
class Array
  alias_method :orig_concat, :concat
  def concat(*xs)
    p yield(self) if block_given?
    orig_concat(*xs)
  end
end
fs = [proc { |x| x.size }, :first]
a = [1, 2]
a.concat([3], &fs[1])
a.concat([4], &fs[0])
p a
