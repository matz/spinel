# a define_singleton_method in a method another define_singleton_method's
# block made: a writer that defines a name the class does not answer yet, or
# that does not redefine, keeps working; a reader alone is inherited
class Base
  def self.setting(name, default)
    define_singleton_method(name) { default }
    define_singleton_method("#{name}=") { |value| value * 2 }
    define_singleton_method("#{name}_mark!") do
      define_singleton_method(:marked?) { true }
    end
  end

  setting :per_page, 25
end

class Items < Base
  self.per_page = 50
  per_page_mark!
end

p Base.per_page, Items.per_page, Base.per_page = 3
p Items.marked?
