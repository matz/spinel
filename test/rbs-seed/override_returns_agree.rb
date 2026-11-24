# spinel: rbs-seed-run
# Identical declarations must retain the boxed return even when every body
# infers Integer. Exercise siblings, an inherited call and a direct call.
class PinBase
  #: (Integer) -> untyped
  def score(x)
    x + 1
  end
  #: (Integer) -> untyped
  def self.score(x)
    x + 2
  end
end
class PinSub < PinBase
  #: (Integer) -> untyped
  def score(x)
    x + 3
  end
  #: (Integer) -> untyped
  def self.score(x)
    x + 4
  end
end
class PinSibling < PinBase
  #: (Integer) -> untyped
  def score(x)
    x + 5
  end
end
class PinLeaf < PinSub
end
[PinBase.new, PinSub.new, PinSibling.new, PinLeaf.new].each { |o| p o.score(10) }
p PinSub.new.score(20)
[PinBase, PinSub].each { |k| p k.score(30) }
p PinSub.score(40)

class PinValues
  #: (bool) -> Integer?
  def maybe(flag)
    flag ? 7 : nil
  end
  #: (String) -> String
  def render(text)
    text
  end
  #: () -> PinValues
  def identity
    self
  end
end
class PinValuesSub < PinValues
  #: (bool) -> Integer?
  def maybe(flag)
    flag ? 9 : nil
  end
  #: (String) -> String
  def render(text)
    text
  end
  #: () -> PinValues
  def identity
    self
  end
end
text = +"kept"
[PinValues.new, PinValuesSub.new].each do |o|
  p o.maybe(true)
  p o.maybe(false)
  answer = o.render(text)
  p answer.equal?(text)
  answer << "!"
  p text
  p o.identity.equal?(o)
end
p PinValuesSub.new.maybe(false)
p PinValuesSub.new.render(text).equal?(text)
