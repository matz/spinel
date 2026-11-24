# spinel: rbs-seed-run
# A conflicting sibling prevents pins on the entire family, including the
# subclass whose declaration agrees with the base.
class ConflictBase
  #: () -> Integer
  def value
    1
  end
end
class ConflictSub < ConflictBase
  #: () -> Integer
  def value
    2
  end
end
class ConflictSibling < ConflictBase
  #: () -> String
  def value
    "three"
  end
end
[ConflictBase.new, ConflictSub.new, ConflictSibling.new].each { |o| p o.value }
p ConflictSub.new.value
p ConflictSibling.new.value

# Equal scalar kinds are insufficient: Integer? uses an optional carrier.
class OptionalBase
  #: () -> Integer
  def value
    4
  end
end
class OptionalSub < OptionalBase
  #: () -> Integer?
  def value
    nil
  end
end
[OptionalBase.new, OptionalSub.new].each { |o| p o.value }
p OptionalSub.new.value
