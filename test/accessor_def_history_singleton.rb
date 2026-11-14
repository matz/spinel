# The class side: a def then `class << self; attr_reader`, and the accessor
# then the def, answer the one live at each call.
class DefThenAttr
  @x = :attr
  def self.x = :def
end
p DefThenAttr.x
class DefThenAttr
  class << self
    attr_reader :x
  end
end
p DefThenAttr.x

class AttrThenDef
  @x = :attr
  class << self
    attr_reader :x
  end
end
p AttrThenDef.x
class AttrThenDef
  def self.x = :def
end
p AttrThenDef.x
