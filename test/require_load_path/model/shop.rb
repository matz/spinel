$shop_loads = ($shop_loads || 0) + 1

class Shop
  def self.hooks = (@hooks ||= [])
  hooks << :tow
end
