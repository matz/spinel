# An empty `{}` local whose `[]=` key is a block's return value.
#
# The key-context pass runs during the fixpoint, where a key that is a block
# result has no type yet, so the local fell through to the post-fixpoint
# backstop -- which defaulted it to StrPolyHash. Codegen reads a StrPolyHash
# key out of the boxed value as `.v.s`, so the write handed a boxed Integer to
# a `const char *` slot and the program segfaulted with no output.
#
# The backstop now checks the local's own `[]=` keys, which by then DO have a
# type, and only keeps the String-keyed default when the key is a String (or
# there are no writes to say otherwise).
#
# Only the receiver form varied: `Rel.new.group { }` was always correct,
# `x = Rel.new; x.group { }` was the crash.

class Rel
  def group
    out = {}
    [1, 2, 3].each do |rec|
      k = yield rec
      arr = out.fetch(k, nil)
      if arr.nil?
        arr = []
        out[k] = arr
      end
      arr << rec
    end
    out
  end

  def single
    out = {}
    k = yield 5
    out[k] = 5
    out
  end

  def str_keyed
    out = {}
    k = yield 5
    out[k.to_s] = 5
    out
  end
end

x = Rel.new
p x.single { |v| v % 2 }
p x.group { |v| v % 2 }
p x.str_keyed { |v| v % 2 }

# the inline-receiver form keeps its precise Hash[Integer, Integer]
p Rel.new.single { |v| v % 2 }

# a block result reached through an explicit block param, same shape
class Blk
  def single(&blk)
    out = {}
    k = blk.call(5)
    out[k] = 5
    out
  end
end
y = Blk.new
p y.single { |v| v % 2 }
