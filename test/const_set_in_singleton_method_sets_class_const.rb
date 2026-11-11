# In a method a `class << self` defines, self is the class, so a bare
# const_set sets the class's constant. It was recorded as the singleton's,
# and every read of the name in the singleton's methods went to that one,
# which nothing had set: NameError even where the setter never ran.
# actionview's ActionView::Template defines `mime_types_implementation=`
# this way beside `Types.valid_symbols?`.
module Mime
  def self.symbols = [:html, :text]
end

class Template
  Types = ::Mime
  LIMIT = 4
  class << self
    # neither setter is called here, as in the application
    def impl=(i)
      if self::Types != i
        remove_const(:Types)
        const_set(:Types, i)
      end
    end

    def syms = Types.symbols

    def reset_limit(n) = const_set(:LIMIT, n)
    def limit = LIMIT + 1
  end
end

p Template.syms
p Template::Types
p Template.limit, Template::LIMIT
