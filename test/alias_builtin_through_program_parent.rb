# An alias of a builtin's method in a class whose program parent derives
# from Hash names the builtin's, not the class's later def of the name
class Base < Hash
end
class M < Base
  alias_method :regular_reader, :[]
  alias_method :regular_writer, :[]=
  def [](key)
    regular_reader(key.to_s)
  end
  def []=(key, value)
    regular_writer(key.to_s, value)
  end
end
m = M.new
m[:a] = 1
p m[:a]
p m

# A parent between them with its own method of the name is the one the alias takes
module NS
  class Base < ::Hash
    def []=(key, value)
      super(key, "base:#{value}")
    end
  end
end
class W < NS::Base
  alias_method :regular_writer, :[]=
  def []=(key, value)
    regular_writer(key.to_s, value)
  end
end
class N < NS::Base
end
class O < N
  alias_method :w, :[]=
  def []=(key, value)
    w(key.to_sym, value)
  end
end
w = W.new
w[:a] = 1
p w
o = O.new
o["b"] = 2
p o
