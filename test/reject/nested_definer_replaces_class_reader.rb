# a nested definer that replaces a method the class answers already, run for a
# subclass: defined from the start it would replace the class's own reader too
class Base
  def self.setting(name, default)
    define_singleton_method(name) { default }
    define_singleton_method("#{name}=") do |value|
      define_singleton_method(name) { 7 }
    end
  end
  setting :per_page, 25
end
class Items < Base
  self.per_page = 50
end
p Base.per_page, Items.per_page
