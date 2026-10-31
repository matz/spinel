# The native branch returns SP_BUILTIN_SYM_ARRAY, not a PolyArray containing
# a Symbol. The reference branch supplies the same Ruby contents.
module NativeSymbolSource
  if RUBY_ENGINE == "spinel"
    native_func :array, [:any], :any, "concat_symbol_source"
  else
    def self.array(x)
      x
    end
  end
end

x = NativeSymbolSource.array([:s])
a = [1, "s"]
a.concat(x)
p a
p a.last.class
a = [1, "s"]
r = a.concat(x)
p r
p r.equal?(a)

a = [1, "s"]
a.concat([2], x)
p a
p a.last.class
a = [1, "s"]
r = a.concat([2], x)
p r
p r.equal?(a)

# A later boxed conversion captures the first operand before appending.
class EmptyConcatSource
  def to_ary
    []
  end
end
y = [EmptyConcatSource.new, 0].first
a = [1, "s"]
a.concat(x, y)
p a
p a.last.class
a = [1, "s"]
r = a.concat(x, y)
p r
p r.equal?(a)
