# a nested definer that replaces a class method the class inherits from its
# superclass, run for a subclass
class Parent
  def self.per_page = 25
end
class Base < Parent
  def self.setting(name)
    define_singleton_method("#{name}=") { |v| define_singleton_method(name) { 7 } }
  end
  setting :per_page
end
class Items < Base
  self.per_page = 1
end
p Base.per_page, Items.per_page
