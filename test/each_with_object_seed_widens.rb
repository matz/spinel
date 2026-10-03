# An each_with_object seed with elements of its own keeps its kind while
# every push fits it; a push of another kind through the block's memo
# widens it to the general Array. The seed ran as an Array[Integer] and
# raised "cannot store AppError into an Array[Integer]" at the push, or the
# program was refused for a String or Float push.
class AppError < StandardError; end

errs = [AppError.new("a"), AppError.new("b")]
r = errs.each_with_object([1]) { |e, acc| acc << e }
p r.map(&:class)
p r.map { |x| x.is_a?(AppError) ? x.message : x }

s = ["x", "y"].each_with_object([1]) { |e, acc| acc << e }
p s

f = [1.5, 2.5].each_with_object([0]) { |e, acc| acc.push(e) }
p f

# a push that fits keeps the typed seed
n = [2, 3].each_with_object([1]) { |e, acc| acc << e * 10 }
p n
p n.sum
