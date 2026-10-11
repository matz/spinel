module Registry
  @all = []
  def self.register(x) = @all << x
  def self.all = @all
end
Registry.register(:a)
ITEMS = Registry.all.dup
