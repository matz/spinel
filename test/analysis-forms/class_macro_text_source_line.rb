# __LINE__ in a module_eval'd text answers the line passed to module_eval,
# not the class body's line the text is grafted at: the text pass refuses
# the call; the form parses the
# text from the line given.
module Defs
  def liner(n)
    module_eval <<-RUBY, __FILE__, __LINE__ + 1
      def #{n} = __LINE__
    RUBY
  end
end
class Box
  extend Defs
  liner :foo
end
p Box.new.foo
