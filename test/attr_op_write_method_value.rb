class Shop
  attr_accessor :customers, :label

  def initialize
    @customers = 0
    @label = "shop"
  end

  def arrive
    self.customers += 1
  end

  def leave = self.customers -= 1

  def rename(suffix)
    self.label += suffix
  end
end

class Meter
  attr_writer :level

  def initialize
    @level = 0
  end

  def level = @level * 10
  def raise_level = self.level += 1
end

shop = Shop.new
p shop.arrive
p shop.arrive
p shop.leave
p shop.rename("!")
p [:arrive, :leave].map { |m| shop.send(m) }
p [shop.customers, shop.label]
meter = Meter.new
p meter.raise_level
p meter.raise_level
p meter.level
