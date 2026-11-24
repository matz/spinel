# spinel: rbs-seed-contradicted-return
# spinel: rbs-seed-no-share
# The external ivar declaration must survive deferred inline return pins.
class ReplayInheritedBase
  def initialize
    @rows = [1]
  end

  #: () -> Array[Integer]
  def rows = @rows
end
class ReplayInheritedBaseSub < ReplayInheritedBase
  #: () -> Array[Integer]
  def rows = @rows
end
[ReplayInheritedBase.new, ReplayInheritedBaseSub.new].each { |obj| p obj.rows }
p ReplayInheritedBaseSub.new.rows
