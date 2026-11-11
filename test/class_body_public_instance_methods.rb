# A class body asks its own class for its method list: bare, on self,
# inherited or not, and in a subclass that adds to its parent's.

module Greets
  def hello = "hi"
end

class Wrapper
  include Greets

  def initialize(obj)
    @obj = obj
  end

  def ==(other) = @obj == other

  def unwrap = @obj

  # the methods a Wrapper answers itself, as the class lists them
  @api = public_instance_methods
  @own = self.public_instance_methods(false)
  @all = instance_methods(true)
  def self.api = @api
  def self.own = @own
  def self.all = @all
end

class LoudWrapper < Wrapper
  def shout = unwrap.to_s.upcase

  @api = public_instance_methods
  def self.api = @api
end

# a module body lists its own methods
module Named
  def label = "n"

  @api = public_instance_methods
  def self.api = @api
end

# a class that defines its own public_instance_methods answers with it
class Listed
  def self.public_instance_methods(all = true) = [:listed]

  def item = 1

  @api = public_instance_methods
  def self.api = @api
end

p Wrapper.api.include?(:unwrap)
p Wrapper.api.include?(:==)
p Wrapper.api.include?(:hello)
p Wrapper.api.include?(:object_id)
p Wrapper.api.include?(:initialize)
p Wrapper.own.sort
p Wrapper.all.include?(:hello)
p LoudWrapper.api.include?(:shout)
p LoudWrapper.api.include?(:unwrap)
p Wrapper.api.include?(:shout)
p Listed.api
p Named.api
p Wrapper.new(3) == 3
