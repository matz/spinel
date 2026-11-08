# A Struct member's reader comes from Struct.new, ahead of the block, so a
# `private` or `protected` section in the block or in a reopen leaves it
# public: `method_defined?(:mem, false)` is true and the guarded reopen
# does not run. A word that names the member still changes it.
S = Struct.new(:mem) do
  private
  def helper = 1
end
unless S.method_defined?(:mem, false)
  class S; def mem = :replaced; end
end
p S.new(5).mem

T = Struct.new(:mem, :other)
class T; private; def priv = 1; end
unless T.method_defined?(:mem, false)
  class T; def mem = :replaced; end
end
p T.new(5).mem

U = Struct.new(:mem, :x) do
  protected
  def y = 1
end
unless U.method_defined?(:mem, false)
  class U; def mem = :replaced; end
end
p U.new(5).mem

V = Struct.new(:mem) do
  def h = 1
  private :mem
end
unless V.method_defined?(:mem, false)
  class V; def mem = :replaced; end
end
p V.new(5).send(:mem)

W = Struct.new(:mem) do
  private def mem = :own
end
unless W.method_defined?(:mem, false)
  class W; def q = 1; end
end
p W.new(5).respond_to?(:q)

# a statement the fold cannot read after the section (an `if` holding a
# def of the name) leaves the answer not known, as before
X = Struct.new(:mem) do
  private
  if true
    def mem = :own
  end
end
unless X.method_defined?(:mem, false)
  class X; def q = 1; end
end
p X.new(5).respond_to?(:q)
