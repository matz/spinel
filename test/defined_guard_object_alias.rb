# A top-level `defined?(X)` guard sees a constant mixed into Object through
# a class body named by a constant that holds Object: `class GuardAlias`
# reopens Object itself.
GuardAlias = Object
module GuardAliasMixin
  AliasMixedConst = :alias_mixed
end
class GuardAlias
  include GuardAliasMixin
end
puts "missing alias mixed" unless defined?(AliasMixedConst)
p AliasMixedConst
