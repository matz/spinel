class TupleAccount
  attr_accessor :units

  def record(count)
    self.units, label = count, +"net"
    [units, label]
  end
end

class TupleBulkAccount < TupleAccount
  def units=(count)
    GC.start
    @units = count * 10
  end
end

p TupleAccount.new.record(3), TupleBulkAccount.new.record(7)
