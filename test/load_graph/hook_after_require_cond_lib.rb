class Cond
  def self.inherited(k) = puts("cond inherited #{k}")
  def self.method_added(n) = puts("cond added #{n}")
end
