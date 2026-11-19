# A program's own NilClass operator answers for nil, a literal or an
# instance variable nothing sets (it raised NoMethodError).
class NilClass
  def +(o) = o * 10
  def <(o) = :lt
end

class Probe
  def initialize = (@b = 1)
  def r = [nil + 2, @x + 3, nil < 1, @x < 1]
end

p Probe.new.r
