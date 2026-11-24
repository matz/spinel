# spinel: rbs-seed-contradicted-return
# spinel: rbs-seed-no-share
# Intentionally false return declarations must be refused, not executed.
# This proves agreement kept the pins: without them both bodies infer Integer.
class FalsePinBase
  def value
    1
  end
end
class FalsePinSub < FalsePinBase
  def value
    2
  end
end
[FalsePinBase.new, FalsePinSub.new].each { |o| p o.value }
