class Base
  def self.inherited(k) = puts("inherited #{k}")
  def self.method_added(n) = puts("added #{n}")
end
class Gone
  def self.singleton_method_removed(n) = puts("removed #{n}")
end
