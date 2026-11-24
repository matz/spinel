# A def a hook's class_eval splices into a core class's reopening reads and
# writes the value's instance variables, as a def written in the reopening
# does; a program's own String in a module is no core class.
module Tagged
  def self.included(base)
    base.class_eval do
      def tag = (@tag ||= "t")
      def tag=(v)
        @tag = v
      end
    end
  end
end
class String
  include Tagged
end
s = +"x"
p s.tag
s.tag = "u"
p s.tag
module Text
  module Tagged2
    def self.included(base)
      base.class_eval do
        def tag = (@tag ||= "t")
      end
    end
  end
  class String
    include Tagged2
  end
end
p Text::String.new.tag
