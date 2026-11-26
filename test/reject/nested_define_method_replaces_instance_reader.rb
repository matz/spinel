# a define_method in a define_singleton_method's block that replaces an instance
# method the class answers, run for a subclass
class Base
  def per_page = 25
  define_singleton_method(:install) { define_method(:per_page) { 7 } }
end
class Items < Base
  install
end
p Base.new.per_page, Items.new.per_page
