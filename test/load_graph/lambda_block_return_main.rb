# A `return` in a `lambda { }` block at a required file's top level leaves
# the lambda, as one in `->(v) { }` does: it is no block's `return`.
require_relative "lambda_block_return_lib"
p Codec::Pairs.call([1, 2, 3, 4])
p Codec::Pairs.call(7)
p Codec::Arrow.call(nil)
p Codec::Arrow.call(5)
