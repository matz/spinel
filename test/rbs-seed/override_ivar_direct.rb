# spinel: rbs-seed-contradicted-return
# spinel: rbs-seed-no-share
# The external ivar declaration must survive deferred inline return pins.
class ReplayDirectBase
  def initialize
    @rows = [1]
  end

  #: () -> Array[Integer]
  def rows = @rows
end
class ReplayDirectBaseSub < ReplayDirectBase
  #: () -> Array[Integer]
  def rows = @rows
end
[ReplayDirectBase.new, ReplayDirectBaseSub.new].each { |obj| p obj.rows }
p ReplayDirectBaseSub.new.rows
