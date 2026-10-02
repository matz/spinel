module Describe
  def describe(tag) = [name, tag]
end

class Class
  include Describe
end

class Base
  def self.label(tag) = "#{name}:#{tag}"
  def label = self.class.label(:x)
  def describe = self.class.describe(:y)
end

class Child < Base
end

[Base.new, Child.new].each do |o|
  p [o.label, o.describe]
end
