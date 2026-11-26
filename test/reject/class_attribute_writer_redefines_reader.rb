# a class_attribute writer that redefines its reader with the value it is given, run
# for a subclass: the nested definer would replace the class's reader before any
# run and read a local of the writer's block, so it is refused (on master it gave nil)
# class_attribute: the setter redefines the reader; a subclass body calls the setter
class Base
  def self.setting(name, default)
    define_singleton_method(name) { default }
    define_singleton_method("#{name}=") do |value|
      define_singleton_method(name) { value }
    end
  end

  setting :per_page, 25
end

class Items < Base
  self.per_page = 50
end

p Base.per_page
p Items.per_page
