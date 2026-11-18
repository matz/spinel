# A statement that runs ahead of a class's first body sees no object of that
# class: a call it makes cannot reach a module mixed into the class later.
# Here a block stored early reads `cache_store` on a boxed value; a module
# with a `cache_store` of its own is included into a reopened module after
# both, and the block runs only once the include has run.
class Registry
  def self.steps = (@steps ||= [])
  def self.step(&blk) = steps << blk
  def self.run = steps.each { |b| b.call }
end

class Config
  def cache_store = :memory
end

things = [Config.new, "x"]
Registry.step { p things[0].cache_store }

module Caching
  module ConfigMethods
    def cache_store = :from_module
  end
end

module Caching
  include ConfigMethods
end

class Controller
  include Caching
end

Registry.run
p Controller.new.cache_store
