# A def with a receiver inside `class << self` is a method of the singleton
# class's own singleton, which takes no inline signature: a malformed one
# above it is only the placement warning, and the receiverless def beside
# it still takes its own.
class C
  class << self
    #: (Integer -> String
    def self.m(x) = x.to_s
    #: (Integer) -> String
    def n(x) = x.to_s
  end
end
p C.n(1)
