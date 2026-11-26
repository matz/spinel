# A send of a computed name on self in an instance method, with a splat,
# two arguments or keywords and a block, does not stop a lambda's return in
# a required file's top level: that self is no class nor main, so the send
# cannot redefine the `lambda` the file calls.
require_relative "computed_send_args_lambda_return_required/codec"

class Box
  def size(n = 0, m = 0, key: 0) = 3 + n + m + key + (block_given? ? yield : 0)
  def ask(name, *args) = send(name, *args)
  def ask2(name) = send(name, 1, 2)
  def ask_kw(name) = send(name, key: 1) { 2 }
end

p Box.new.ask(:size, 1)
p Box.new.ask2(:size)
p Box.new.ask_kw(:size)
p Pairs.call([1, 2, 3, 4])
p Pairs.call(7)
