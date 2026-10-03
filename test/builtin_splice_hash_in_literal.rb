# A `#` that starts no comment hides no builtin: after one in a string
# literal, an interpolation's `#{`, a %w list, a regexp, a character literal
# or a class_eval template, the call on the same line still gets the
# Enumerable written in Ruby. Every such call here stands behind a `#`.
puts "issue #12"; p [1, 2, 3].partition { |v| v > 1 }
n = 3
puts "n=#{n} parts=#{[1, 2, 3, 4].partition { |v| v > n - 1 }}"
puts 'single #1'; p %w[pear fig apple].min_by { |s| s.size }
p %w[# a bb].max_by { |s| s.size }
p "a#b".index(/#/), [3, 1, 2].minmax_by { |v| -v }
p ?#, [1, 2, 3].each_with_object([]) { |v, acc| acc << v * 2 }
text = <<~EOS
  heredoc #{[4, 5, 6].find_index { |v| v > 4 }}
EOS
puts text

class Bag
  def initialize = @list = [1, 2, 3, 4]
  KINDS = %w[big small]
  KINDS.each do |kind|
    class_eval <<~RUBY
      def #{kind}_parts = @list.partition { |v| v > 2 }
    RUBY
  end
end
p Bag.new.big_parts
p Bag.new.small_parts
