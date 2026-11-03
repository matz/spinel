module Store
  SLOTS = {}
  def self.get(key) = SLOTS[key]
  def self.set(key, value) = SLOTS[key] = value
  def self.fetch(key, &block)
    get(key) || set(key, block.call)
  end
end

def intern(css)
  pool = Store.fetch(:pool) { {} }
  cached = pool[css]
  return cached if cached

  pool[css] = css
end

class Ctx
  def initialize(n) = @n = n
  def n = @n
end

p Store.fetch(:count) { 3 }
p Store.fetch(:name) { "kale" }
p Store.fetch(:ctx) { Ctx.new(2) }.n
p Store.fetch(:list) { [1, 2] }
p intern("a: b")
p intern("a: b").equal?(intern("a: b"))
