# A return annotation on a method that a subclass overrides is not applied
# (as with --rbs), and inline RBS says so; the parameter annotation still is.
class Base
  #: (untyped) -> untyped
  def size(x)
    x
  end
end

class Wide < Base
  def size(x)
    x * 2
  end
end

[Base.new, Wide.new].each { |o| p o.size(3) }

# The complete family agrees; untyped must remain boxed, even though every
# body infers Integer. A sibling is part of the same family as the child.
class AgreedBase
  #: (Integer) -> untyped
  def score(x)
    x + 1
  end
end
class AgreedSub < AgreedBase
  #: (Integer) -> untyped
  def score(x)
    x + 2
  end
end
class AgreedSibling < AgreedBase
  #: (Integer) -> untyped
  def score(x)
    x + 3
  end
end
[AgreedBase.new, AgreedSub.new, AgreedSibling.new].each { |o| p o.score(10) }
p AgreedSub.new.score(20)
