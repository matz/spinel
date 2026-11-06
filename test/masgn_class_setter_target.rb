# A multiple assignment whose target is a setter on a class or module
# (`old, I18n.config = I18n.config, proxy`, which Rails' rendering writes
# to swap a config for the length of a call).

module Cfg
  class << self
    attr_accessor :config
  end
end

module Store
  def self.value = @value
  def self.value=(v)
    @value = v
    puts "set #{v.inspect}"
  end
end

Cfg.config = 1
old, Cfg.config = Cfg.config, 2
p old, Cfg.config

Store.value = :a
o2, Store.value = Store.value, :b
p o2, Store.value

def swap(v)
  prev, Cfg.config = Cfg.config, v
  yield
ensure
  Cfg.config = prev
end
swap(10) { p Cfg.config }
p Cfg.config

Cfg.config, Store.value, x = 7, :c, 3
p Cfg.config, Store.value, x
