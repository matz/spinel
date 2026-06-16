# Issue #1314. `specialize_inherited_cls_new` clones an inherited class
# method that does a bare `new` (or a subclass-specific cmethod call)
# into each subclass that calls it, then DCEs the now-shadowed source
# unless it is called directly as `<DefiningClass>.<name>`. The
# "was this specialized?" test used to match by method *name* only, so
# specializing `Base.find` into `Article` also flagged the completely
# unrelated `Store.find` (same name, different class) as a transplanted
# source and dropped it from emission. When `Store.find` was still
# reachable and called through a non-constant receiver (here a bare
# sibling call, which is not "called directly as Store.find"), its body
# was never emitted while the call site survived -- the generated C then
# referenced an undefined `sp_Store_s_find`, failing to compile.
#
# The fix narrows the match to require the appended copy to live in a
# *subclass* of the source's defining class, so unrelated same-named
# class methods are left alone.

class Base
  def self.find(x)
    new
  end
  def initialize
    @v = 7
  end
  attr_reader :v
end

# A subclass that inherits Base.find: `Article.find` triggers the
# bare-`new` specialization, appending an `Article`-owned `find` copy.
class Article < Base
end

# An unrelated module with its own same-named `find`. It must NOT be
# treated as a specialization of Base.find.
module Store
  def self.find(x)
    x * 2
  end
  def self.lookup(x)
    # Bare sibling call -> resolves to Store.find, emitted as
    # sp_Store_s_find, but the call has no constant receiver, so the
    # DCE's "called_direct" guard does not protect it.
    find(x)
  end
end

p Article.find(1).v
p Store.lookup(21)
