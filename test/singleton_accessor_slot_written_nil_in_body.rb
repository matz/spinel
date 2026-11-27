# A class body sets a class-level ivar to nil and gives it a singleton
# accessor: `@engine = nil` then `class << self; attr_accessor :engine; end`,
# a setting another class's body writes later (`Arel::Table.engine = self`
# in a load hook). The body's nil write made the accessor read the civ_ slot,
# which only a typed write declared: with nil alone the slot was never
# declared, and clang stopped at the undeclared identifier.
module Arel
  class Table
    @engine = nil
    class << self; attr_accessor :engine; end
    def initialize(name) = @name = name
    def engine = self.class.engine
  end
end
module ActiveRecord
  class Base
    def self.hello = "base"
  end
end
p Arel::Table.engine
class ::ActiveRecord::Base
  Arel::Table.engine = self
end
p Arel::Table.engine, Arel::Table.new("users").engine.hello

# the nil written first, the accessor declared after; and a class method
# reading the slot beside the accessor
class Config
  class << self; attr_accessor :store; end
  @store = nil
  def self.store? = !@store.nil?
end
p Config.store, Config.store?
Config.store = { a: 1 }
p Config.store, Config.store?
