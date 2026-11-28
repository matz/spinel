# A class_eval text grafted where only the walk reaches it (an included
# hook's body) may call a builtin whose implementation is Ruby: the call's
# site takes its copy of that implementation as a text at the top level does.
module Sorted
  def self.included(base)
    base.class_eval <<~RUBY
      def halves
        @items.partition { |x| x.even? }
      end

      def pairs
        @items.each_slice(2).to_a
      end
    RUBY
  end
end

class Bag
  include Sorted
  def initialize(items) = @items = items
end

b = Bag.new([1, 2, 3, 4, 5])
p b.halves
p b.pairs
