class Base
  def self.inherited(k) = puts("inherited #{k}")
  def self.method_added(n) = puts("added #{n}")
end
