# spinel: rbs-seed-run
# An unannotated grandchild invalidates agreement even on the ancestor and
# sibling. Leaving one of these untyped returns pinned would hide the gap.
class MissingBase
  #: () -> untyped
  def value
    1
  end
end
class MissingSub < MissingBase
  #: () -> untyped
  def value
    2
  end
end
class MissingSibling < MissingBase
  #: () -> untyped
  def value
    3
  end
end
class MissingLeaf < MissingSub
  def value
    4
  end
end
[MissingBase.new, MissingSub.new, MissingSibling.new, MissingLeaf.new].each { |o| p o.value }
p MissingSub.new.value
p MissingLeaf.new.value
