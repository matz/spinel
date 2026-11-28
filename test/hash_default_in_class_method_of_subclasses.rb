# ActiveRecord::Base._load_from_sql: inside an instrument block of a class
# method that runs for several classes, a Hash whose default calls a class
# method on self with a Hash argument, read through a lambda and a block
class Bus
  def instrument(name, payload)
    yield
  end
end
module Cm
  module ClassMethods
    def load_from(rows)
      bus = Bus.new
      bus.instrument("load", { count: rows.size, class_name: name }) do
        if rows.first.is_a?(String)
          classes = Hash.new do |h, type|
            h[type] = discriminate(inheritance_column => type)
          end
          map_for = ->(type) { classes[type].label }
          rows.map { |type| "#{map_for.call(type)}/#{classes[type].label}" }
        else
          rows.map { |r| "#{label}:#{r}" }
        end
      end
    end
    def inheritance_column = "type"
    def discriminate(attrs) = (attrs["type"] == "sub" ? Sub : self)
    def label = name.downcase
  end
  def self.included(base) = base.extend(ClassMethods)
end
class Base
  include Cm
end
class Sub < Base
  def self.inheritance_column = "kind"
end
p Base.load_from(["base", "sub", "base"]), Sub.load_from([1, 2]), Sub.load_from(["sub"])
